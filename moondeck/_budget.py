"""The machine's job budget, handed out through GNU make's jobserver protocol, shared by every process a run starts.

One budget per run: `build_host.build_jobs()` of them, the cores less two and capped by memory, so MoonLight, the editor and the browser
keep their cores. The tokens sit in a named pipe as make's jobserver keeps them (the `fifo:` form of `--jobserver-auth`, make 4.4 and
ninja 1.13): a process holds one implicit token and reads one byte from the pipe for each further job, writing it back when the job ends.
`MAKEFLAGS` announces the pipe to every child, so a nested run joins the same budget instead of opening its own. A build keeps its own
job count (`--parallel`, `IDF_PY_BUILD_JOBS`), which takes it out of the pipe, and macOS's make 3.81 reads no `fifo:` pipe at all:
the budget bounds how many gates run at once, and a build beside them still runs its own count.

stdlib only and cheap to import, as `_host.py` is, so a PEP-723 script imports it after adding moondeck/ to sys.path.
"""

import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "build"))
from build_host import build_jobs  # noqa: E402  (the one bound on jobs)


class JobBudget:
    """`tokens` jobs at once, through a jobserver pipe of `tokens - 1` bytes plus the implicit one; joins the pipe `MAKEFLAGS` names."""

    def __init__(self, tokens: int | None = None):
        self.tokens = max(1, tokens or build_jobs())
        self._dir = None
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
            os.write(self._fd, b"+" * (self.tokens - 1))

    def acquire(self) -> None:
        """Take one token beyond the implicit one, waiting until a job ends."""
        os.read(self._fd, 1)

    def release(self) -> None:
        """Give a token back."""
        os.write(self._fd, b"+")

    def env(self) -> dict:
        """The environment a child runs in, its MAKEFLAGS naming this budget's pipe."""
        return {**os.environ, "MAKEFLAGS": f"-j{self.tokens} --jobserver-auth=fifo:{self.path}"}

    def close(self) -> None:
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
