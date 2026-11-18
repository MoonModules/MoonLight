"""build_host.lower_priority: a build yields the CPU, and one started already at the lowest priority runs on rather than failing."""

import os

import build_host


def test_a_build_yields_the_cpu_and_one_already_at_the_lowest_priority_runs_on(monkeypatch):
    asked = []
    monkeypatch.setattr(os, "nice", lambda by: asked.append(by) or 10, raising=False)
    build_host.lower_priority()
    assert asked == [10]

    def refuse(by):   # macOS answers a step past the lowest priority with EPERM rather than clamping
        raise PermissionError(1, "Operation not permitted")
    monkeypatch.setattr(os, "nice", refuse, raising=False)
    build_host.lower_priority()   # runs on
