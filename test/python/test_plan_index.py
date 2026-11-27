"""The plan index: a plan's steps counted from their markers, so a plan's name stays its date and topic. Run: `uv run moondeck/test/test_tooling.py --python`."""

import pytest

import plan_index
from plan_index import count_steps


def counts(text):
    c = count_steps(text)
    return c["🚧"], c["🧪"], c["✅"], c["⏳"]


def test_step_headings_are_each_counted_by_their_marker():
    text = "## Steps\n\n### Step 1: first ✅\n\n### Step 2: second 🚧\n\n### Step 3: third ⏳\n"
    assert counts(text) == (1, 0, 1, 1)


def test_numbered_items_are_each_counted():
    text = "## Steps\n\n1. ✅ one\n2. 🧪 two\n3. ⏳ three\n"
    assert counts(text) == (0, 1, 1, 1)


def test_phase_lines_are_each_counted():
    text = "## Phases\n\n✅ **Phase 1: base**\n\n🚧 **Phase 2: more**\n\n⏳ **Phase 3: rest**\n"
    assert counts(text) == (1, 0, 1, 1)


def test_table_rows_are_each_counted_and_a_row_with_no_marker_is_not_started():
    text = "## Steps\n\n| # | Step | Status |\n|--|--|--|\n| 1 | one | ✅ |\n| 2 | two | 🚧 |\n| 3 | three | |\n"
    assert counts(text) == (1, 0, 1, 1)


def test_a_table_with_no_marker_ahead_of_marked_step_headings_leaves_the_headings_as_the_steps():
    text = "## Steps\n\n| # | Area | Notes |\n|--|--|--|\n| 1 | core | |\n| 2 | light | |\n\n### Step 1: Implement ✅\n\n### Step 2: Verify 🧪\n"
    assert counts(text) == (0, 1, 1, 0)


def test_a_table_whose_rows_all_wait_counts_each_as_not_started():
    text = "## Steps\n\n| # | Step | Status |\n|--|--|--|\n| 1 | one | |\n| 2 | two | |\n"
    assert counts(text) == (0, 0, 0, 2)


def test_only_the_first_marked_structure_counts_so_checks_under_a_phase_stay_out():
    text = "## Phases\n\n✅ **Phase 1: base**\n\n1. ✅ a check\n2. ✅ another check\n\n⏳ **Phase 2: rest**\n\n1. ⏳ a check\n"
    assert counts(text) == (0, 0, 1, 1)


def test_dropped_steps_leave_the_total():
    text = "## Steps\n\n### Step 1: kept ✅\n\n### Step 2: dropped ❌\n\n### Step 3: paused ⏸\n"
    assert counts(text) == (0, 0, 1, 0)



def test_a_heading_nested_under_a_step_heading_is_no_step_of_its_own():
    text = "## Steps\n\n### Step 1: implement ✅\n\n#### 1. verification ✅\n\n### Step 2: integrate ⏳\n"
    assert counts(text) == (0, 0, 1, 1)


def test_an_example_inside_a_fenced_block_is_no_step():
    text = "## Steps\n\n1. ✅ one\n\n```md\n1. ✅ example\n```\n\n2. ⏳ two\n"
    assert counts(text) == (0, 0, 1, 1)


def test_a_link_encodes_every_character_a_url_reserves(work):
    (work / "Plan-20260101 - C# support.md").write_text("## Steps\n\n1. ✅ one\n", encoding="utf-8")
    assert "(Plan-20260101%20-%20C%23%20support.md)" in plan_index.render()

@pytest.fixture
def work(tmp_path, monkeypatch):
    monkeypatch.setattr(plan_index, "ROOT", tmp_path)
    monkeypatch.setattr(plan_index, "WORK", tmp_path)
    monkeypatch.setattr(plan_index, "INDEX", tmp_path / "index.md")
    (tmp_path / "Plan-20260101 - A topic.md").write_text("## Steps\n\n1. ✅ one\n2. ⏳ two\n", encoding="utf-8")
    return tmp_path


def test_check_passes_on_a_fresh_index_and_fails_on_a_stale_one(work, monkeypatch, capsys):
    monkeypatch.setattr("sys.argv", ["plan_index.py"])
    assert plan_index.main() == 0
    monkeypatch.setattr("sys.argv", ["plan_index.py", "--check"])
    assert plan_index.main() == 0
    (work / "Plan-20260101 - A topic.md").write_text("## Steps\n\n1. ✅ one\n2. ✅ two\n", encoding="utf-8")
    assert plan_index.main() == 1
    assert "FAIL" in capsys.readouterr().out
