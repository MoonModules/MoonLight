"""The Project mirror: the record an agent searching build/project.json relies on, and the documents it renders to docs/work/backlog_mirror/. Run: `uv run moondeck/test/test_host.py --python`."""

from project_mirror import FIELDS, REPO_URL, dead_links, differences, missing_targets, normalize, relative_links, render


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


def test_render_writes_one_document_per_area_named_modules_first_in_board_order():
    raw = {"items": [item("Second", area="Core", module="NetworkModule", kind="Bug"),
                     item("Loose", area="Core", kind="Feature"),
                     item("First", area="Core", module="NetworkModule", kind="Feature", platform="P4"),
                     item("A UI card", area="UI", kind="Feature", platform="all")]}
    docs = render(normalize(raw), "https://example.com/p")
    assert set(docs) == {"core.md", "ui.md", "cards.json"}
    core = docs["core.md"]
    assert core.index("## NetworkModule") < core.index("## Across modules")
    assert core.index("### Second") < core.index("### First")      # board order, not title order
    assert "Kind: Feature · Platform: P4" in core and "### Loose" in core
    assert "Platform: all" not in docs["ui.md"]            # the default is left out, as the files do


def test_a_done_card_leaves_the_documents():
    docs = render(normalize({"items": [item("Shipped", area="Core", status="Done"), item("Open", area="Core", status="Backlog")]}), "u")
    assert "### Open" in docs["core.md"] and "Shipped" not in docs["core.md"]


def test_a_cards_repo_links_become_relative_to_the_backlog_folder():
    body = f"[a]({REPO_URL}docs/contributing/lessons.md#x) [b]({REPO_URL}src/core/Foo.h) [c](https://example.com)"
    out = relative_links(body, "docs/work/backlog_mirror")
    assert "[a](../../contributing/lessons.md#x)" in out and "[b](../../../src/core/Foo.h)" in out and "[c](https://example.com)" in out


def test_check_names_a_changed_a_missing_and_a_leftover_document(tmp_path):
    (tmp_path / "core.md").write_text("old\n"); (tmp_path / "gone.md").write_text("x\n")
    assert differences({"core.md": "new\n", "ui.md": "u\n"}, tmp_path) == ["core.md", "gone.md", "ui.md"]
    (tmp_path / "core.md").write_text("new\n"); (tmp_path / "ui.md").write_text("u\n"); (tmp_path / "gone.md").unlink()
    assert differences({"core.md": "new\n", "ui.md": "u\n"}, tmp_path) == []


def test_a_card_carries_its_url_and_the_documents_link_it():
    raw = {"items": [item("Linked", area="Core", kind="Bug")]}
    rec = normalize(raw, {"PVTI_Linked": "123"}, "https://github.com/users/O/projects/5")[0]
    assert rec["card"] == "https://github.com/users/O/projects/5?pane=issue&itemId=123"
    assert "[Card](https://github.com/users/O/projects/5?pane=issue&itemId=123)" in render([rec], "u")["core.md"]


def test_a_link_to_a_gone_card_or_into_the_mirror_fails(tmp_path):
    live = [{"card": "https://github.com/users/O/projects/5?pane=issue&itemId=123"}]
    (tmp_path / "ok.md").write_text("[a](https://github.com/users/O/projects/5?pane=issue&itemId=123)\n")
    (tmp_path / "bad.md").write_text("[b](https://github.com/users/O/projects/5?pane=issue&itemId=999) [c](../backlog_mirror/core.md#x) [d](backlog_mirror/ui.md)\n")
    found = dead_links(live, tmp_path)
    assert len(found) == 3 and all("bad.md" in f for f in found)
    assert any("999 is not on the board" in f for f in found) and sum("link the card or the Project instead" in f for f in found) == 2


def test_an_open_cards_link_to_a_file_the_repository_lacks_fails_and_a_generated_catalog_page_counts(tmp_path):
    (tmp_path / "src").mkdir(); (tmp_path / "src" / "Foo.h").write_text("")
    (tmp_path / "Plan (1 of 2).md").write_text("")
    cards = [{"title": "Good", "status": "Backlog", "body": f"[a]({REPO_URL}src/Foo.h#L3) [p]({REPO_URL}Plan%20(1%20of%202).md)"},
             {"title": "Catalog", "status": "Backlog", "body": f"[g]({REPO_URL}docs/moonmodules/light/drivers.md#hue)"},
             {"title": "Bad", "status": "Backlog", "body": f"[b]({REPO_URL}docs/src/Foo.h)"},
             {"title": "Shipped", "status": "Done", "body": f"[c]({REPO_URL}gone.h)"}]
    found = missing_targets(cards, tmp_path, {"docs/moonmodules/light/drivers.md"})
    assert found == ["card 'Bad' links docs/src/Foo.h, which the repository does not hold"]


def test_the_data_file_lists_the_open_cards_in_a_stable_order_without_a_timestamp():
    import json
    raw = {"items": [item("Zed", area="UI", kind="Bug", process="install"), item("Alpha", area="Core", kind="Feature"),
                     item("Shipped", area="Core", status="Done")]}
    rows = json.loads(render(normalize(raw), "u")["cards.json"])
    assert [r["title"] for r in rows] == ["Alpha", "Zed"] and rows[1]["process"] == "install"
    assert "generated" not in render(normalize(raw), "u")["cards.json"]
