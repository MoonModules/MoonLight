"""build_host.drop_stale_precompiled_headers: a compiler updated in place leaves precompiled headers clang refuses, so they go, and an unchanged compiler keeps them."""

import os
import stat

import pytest

import build_host


def _tree(tmp_path, version):
    """A build directory whose cache names a compiler that answers `version`, holding one precompiled header."""
    compiler = tmp_path / "fake-cxx"
    compiler.write_text(f"#!/bin/sh\necho '{version}'\n")
    compiler.chmod(compiler.stat().st_mode | stat.S_IEXEC)
    bdir = tmp_path / "build"
    bdir.mkdir(exist_ok=True)
    (bdir / "CMakeCache.txt").write_text(f"CMAKE_CXX_COMPILER:FILEPATH={compiler}\n")
    pch = bdir / "test" / "cmake_pch.hxx.pch"
    pch.parent.mkdir(exist_ok=True)
    pch.write_text("x")
    return bdir, pch


@pytest.mark.skipif(os.name == "nt", reason="the stand-in compiler is a shell script")
def test_a_compiler_updated_in_place_removes_the_precompiled_headers_and_an_unchanged_one_keeps_them(tmp_path):
    bdir, pch = _tree(tmp_path, "clang 17")
    build_host.drop_stale_precompiled_headers(bdir)   # the first run only records the compiler
    assert pch.exists()
    build_host.drop_stale_precompiled_headers(bdir)   # the same compiler: kept
    assert pch.exists()
    _tree(tmp_path, "clang 18")                       # an update under the same path
    build_host.drop_stale_precompiled_headers(bdir)
    assert not pch.exists()
    assert (bdir / "compiler-version.txt").read_text().strip() == "clang 18"
