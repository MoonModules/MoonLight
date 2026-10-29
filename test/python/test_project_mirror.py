"""The Project mirror's record shape: what an agent searching build/project.json can rely on. Run: `uv run moondeck/test/test_host.py --python`."""

from project_mirror import FIELDS, normalize


def item(title, area="", **fields):
    return {"id": f"PVTI_{title}", "title": title, "area": area, **fields,
            "content": {"type": "DraftIssue", "title": title, "body": f"body of {title}"}}


def test_every_record_carries_every_field_empty_when_the_card_leaves_it_unset():
    rec = normalize({"items": [item("A card", area="Core", kind="Bug")]})[0]
    assert all(f in rec for f in FIELDS)
    assert rec["kind"] == "Bug" and rec["stage"] == "" and rec["process"] == ""


def test_a_draft_item_keeps_its_body_and_has_no_issue_number_or_url():
    rec = normalize({"items": [item("Draft")]})[0]
    assert rec["type"] == "DraftIssue" and rec["body"] == "body of Draft"
    assert rec["number"] is None and rec["url"] == ""


def test_an_issue_carries_its_number_and_url():
    raw = {"items": [{"id": "PVTI_1", "title": "An issue", "status": "Next",
                      "content": {"type": "Issue", "title": "An issue", "number": 42,
                                  "url": "https://github.com/MoonModules/MoonLight/issues/42", "body": ""}}]}
    rec = normalize(raw)[0]
    assert rec["number"] == 42 and rec["url"].endswith("/issues/42") and rec["status"] == "Next"


def test_records_sort_by_area_then_title_so_an_unchanged_board_writes_the_same_file():
    raw = {"items": [item("beta", area="UI"), item("Alpha", area="UI"), item("Zed", area="Core")]}
    assert [r["title"] for r in normalize(raw)] == ["Zed", "Alpha", "beta"]


def test_an_empty_board_mirrors_to_no_items():
    assert normalize({}) == [] and normalize({"items": []}) == []
