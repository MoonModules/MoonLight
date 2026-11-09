"""A failed `new (std::nothrow)` returns null on the ESP32 rather than aborting the board.

ESP-IDF links libstdc++ statically, and with C++ exceptions off it makes a throw abort(); libstdc++'s nothrow new calls the
throwing one, so a failed nothrow allocation aborted. platform_esp32.cpp replaces the two nothrow forms with malloc. This
links a program the board's way on the host, the throw wrapped into abort(), and checks that the stock nothrow new aborts
while the platform's two operators return null. GNU ld alone wraps a symbol, so the test runs where it links that way.
"""
import re
import shutil
import signal
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
CXX = shutil.which("g++") or shutil.which("c++")

PROGRAM = r'''
#include <cstdlib>
#include <new>
extern "C" void* __wrap___cxa_allocate_exception(std::size_t) { std::abort(); }   // the board's throw: an abort
int main() {
    volatile std::size_t huge = static_cast<std::size_t>(-1) / 4;                   // more than any address space holds
    char* many = new (std::nothrow) char[huge];                  // the array form
    void* one = ::operator new(huge, std::nothrow);               // and the scalar form
    return many || one ? 1 : 0;
}
'''


def build(tmp_path: Path, name: str, source: str):
    """Compile and link `source` the board's way; the result of the compiler, and the program."""
    cpp, exe = tmp_path / f"{name}.cpp", tmp_path / name
    cpp.write_text(source)
    built = subprocess.run([CXX, "-std=c++20", "-fno-exceptions", "-static-libstdc++", "-Wl,--wrap=__cxa_allocate_exception",
                            str(cpp), "-o", str(exe)], capture_output=True, text=True, timeout=120)
    return built, exe


@pytest.mark.skipif(CXX is None, reason="no C++ compiler")
def test_a_failed_nothrow_allocation_returns_null_rather_than_aborting(tmp_path):
    source = (ROOT / "src/platform/esp32/platform_esp32.cpp").read_text()
    replaced = re.findall(r"^void\* operator new(?:\[\])?\(std::size_t size, const std::nothrow_t&\) noexcept \{.*\}$", source, re.M)
    assert len(replaced) == 2, "platform_esp32.cpp replaces both nothrow forms"
    stock, stock_exe = build(tmp_path, "stock", PROGRAM)
    if stock.returncode != 0:   # only the toolchain decides a skip: GNU ld alone wraps a symbol
        pytest.skip(f"this toolchain cannot link the board's way: {stock.stderr.strip()[:200]}")
    assert subprocess.run([str(stock_exe)], capture_output=True, timeout=20).returncode == -signal.SIGABRT   # the board before: abort
    fixed, fixed_exe = build(tmp_path, "replaced", "#include <cstdlib>\n#include <new>\n" + "\n".join(replaced) + PROGRAM)
    assert fixed.returncode == 0, fixed.stderr                                                    # the platform's code compiles
    assert subprocess.run([str(fixed_exe)], capture_output=True, timeout=20).returncode == 0     # after: null, both forms
