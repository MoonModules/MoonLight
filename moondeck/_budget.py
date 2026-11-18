"""The machine's job budget, handed out through GNU make's jobserver protocol, shared by every process a run starts.

One budget per run: `build_host.build_jobs()` of them, the cores less two and capped by memory, so MoonLight, the editor and the browser
keep their cores. The tokens sit in a named pipe as make's jobserver keeps them (the `fifo:` form of `--jobserver-auth`, make 4.4 and
ninja 1.13): a process holds one implicit token and reads one byte from the pipe for each further job, writing it back when the job ends.
`MAKEFLAGS` announces the pipe to every child, so a nested run joins the same budget instead of opening its own. A build keeps its own
job count (`--parallel`, `IDF_PY_BUILD_JOBS`), which takes it out of the pipe, and macOS's make 3.81 reads no `fifo:` pipe at all:
the budget bounds how many gates run at once, and a build beside them still runs its own count. Windows has no named pipe of this kind
(`os.mkfifo` is POSIX), so there the budget is a semaphore inside the run: the gates still share it, and a child cannot join it.

stdlib only and cheap to import, as `_host.py` is, so a PEP-723 script imports it after adding moondeck/ to sys.path.
"""

import os
import shutil
import sys
import tempfile
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "build"))
from build_host import build_jobs  # noqa: E402  (the one bound on jobs)


class JobBudget:
    """`tokens` jobs at once, through a jobserver pipe of `tokens - 1` bytes plus the implicit one; joins the pipe `MAKEFLAGS` names.

    A `scheduler` runs no job itself, as the gate runner only dispatches: its implicit token goes in the pipe too, so the jobs it starts
    take all `tokens` from there, each as many as it needs at once (`try_acquire`).
    """

    def __init__(self, tokens: int | None = None, scheduler: bool = False):
        self.tokens = max(1, tokens or build_jobs())
        self._dir = None
        self._sem = None
        own = self.tokens - (0 if scheduler else 1)
        if not hasattr(os, "mkfifo"):   # Windows: the run's own semaphore, which no child sees
            self.path, self._fd, self._sem = None, None, threading.Semaphore(own)
            return
        joined = _announced_fifo(os.environ.get("MAKEFLAGS", ""))
        if joined:
            self.path = joined
        else:
            self._dir = tempfile.mkdtemp(prefix="mm-jobs-")
            self.path = os.path.join(self._dir, "jobserver")
            os.mkfifo(self.path)
        # Read and write on one descriptor, so opening returns at once, with this process as both ends.
        self._fd = os.open(self.path, os.O_RDWR)
        if not joined:
            os.write(self._fd, b"+" * own)

    def acquire(self) -> None:
        """Take one token beyond the implicit one, waiting until a job ends."""
        if self._sem:
            self._sem.acquire()
            return
        os.read(self._fd, 1)

    def try_acquire(self, n: int) -> bool:
        """Take all `n` tokens or none, at once, so two jobs each holding part of what they need cannot deadlock."""
        got = 0
        if self._sem:
            while got < n and self._sem.acquire(blocking=False):
                got += 1
        else:
            os.set_blocking(self._fd, False)
            try:
                while got < n:
                    os.read(self._fd, 1)
                    got += 1
            except BlockingIOError:
                pass
            finally:
                os.set_blocking(self._fd, True)
        if got == n:
            return True
        for _ in range(got):
            self.release()
        return False

    def release(self) -> None:
        """Give a token back."""
        if self._sem:
            self._sem.release()
            return
        os.write(self._fd, b"+")

    def env(self) -> dict:
        """The environment a child runs in, its MAKEFLAGS naming this budget's pipe; unchanged where the budget has no pipe."""
        if self._sem:
            return dict(os.environ)
        return {**os.environ, "MAKEFLAGS": f"-j{self.tokens} --jobserver-auth=fifo:{self.path}"}

    def close(self) -> None:
        if self._fd is None:
            return
        os.close(self._fd)
        if self._dir:
            shutil.rmtree(self._dir, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def _announced_fifo(makeflags: str) -> str | None:
    """The jobserver pipe a parent's MAKEFLAGS announces, or None."""
    for word in makeflags.split():
        if word.startswith("--jobserver-auth=fifo:"):
            path = word.split(":", 1)[1]
            return path if os.path.exists(path) else None
    return None
