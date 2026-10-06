#!/usr/bin/env python3
"""Compile one file and keep what the compiler said about it beside the object, as `<object>.diag`.

CMake runs this as `CXX_COMPILER_LAUNCHER`, the hook ccache uses, so the build itself is unchanged.
An incremental build only recompiles what changed and prints nothing for the rest, so the whole tree's
findings live in these files instead: `check_nonblocking.py` reads them after an incremental build rather
than rebuilding the tree clean to make every file speak again.
"""

import subprocess
import sys
from pathlib import Path


def object_path(args):
    """The object file a compile writes, from its `-o`, or None for a command that writes none."""
    for i, a in enumerate(args):
        if a == "-o" and i + 1 < len(args):
            return args[i + 1]
        if a.startswith("-o") and len(a) > 2:
            return a[2:]
    return None


def main(argv):
    proc = subprocess.run(argv, stderr=subprocess.PIPE)
    sys.stderr.buffer.write(proc.stderr)   # the build log reads as without the launcher
    obj = object_path(argv)
    # Written even when empty and even on failure, so its presence says this file's findings were captured.
    if obj:
        Path(obj + ".diag").write_bytes(proc.stderr)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
