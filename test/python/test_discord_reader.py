"""The Discord reader: which channels it reads, which messages a window holds, and how a message reads. Run: `uv run moondeck/test/test_host.py --python`."""

import io
import urllib.error
from datetime import datetime, timezone
from pathlib import Path

import pytest

import discord_reader as dr


def msg(mid, ts, content="hi", **extra):
    return {"id": mid, "timestamp": ts, "content": content, "author": {"username": "anna"}, **extra}


def utc(s):
    return datetime.fromisoformat(s).replace(tzinfo=timezone.utc)


def serve(monkeypatch, routes):
    """Answer _get from a table of path to result; a result that is an exception is raised."""
    calls = []

    def fake(path, **params):
        calls.append((path, params))
        r = routes[path]
        r = r(params) if callable(r) else r
        if isinstance(r, Exception):
            raise r
        return r
    monkeypatch.setattr(dr, "_get", fake)
    return calls


def forbidden():
    return urllib.error.HTTPError("u", 403, "Forbidden", {}, io.BytesIO(b"{}"))


GUILD_CHANNELS = [
    {"id": "c1", "type": 4, "name": "MoonLight", "position": 0},
    {"id": "c2", "type": 4, "name": "GitHub Activity", "position": 1},
    {"id": "t1", "type": 0, "name": "moonlight-dev", "parent_id": "c1", "position": 2},
    {"id": "t2", "type": 0, "name": "moonlight-github", "parent_id": "c2", "position": 3},
    {"id": "v1", "type": 2, "name": "voice", "parent_id": "c1", "position": 4},
    {"id": "f1", "type": 15, "name": "forum", "position": 5},
    {"id": dr.SUMMARY_CHANNEL, "type": 0, "name": "moonlight-daily", "parent_id": "c1", "position": 6},
]


def test_it_reads_text_announcement_and_forum_channels_named_with_their_category_and_skips_the_github_feeds(monkeypatch):
    serve(monkeypatch, {f"/guilds/{dr.GUILD}/channels": GUILD_CHANNELS})
    assert dr._channels() == {"t1": "MoonLight / #moonlight-dev", "f1": "#forum",
                              dr.SUMMARY_CHANNEL: "MoonLight / #moonlight-daily"}


def test_a_window_holds_the_messages_from_its_start_up_to_its_end_oldest_first(monkeypatch):
    page = [msg("4", "2026-10-30T06:00:00+00:00"), msg("3", "2026-10-30T04:59:00+00:00"),
            msg("2", "2026-10-29T05:00:00+00:00"), msg("1", "2026-10-29T04:00:00+00:00")]
    serve(monkeypatch, {"/channels/t1/messages": page})
    got = dr._messages("t1", utc("2026-10-29T05:00:00"), utc("2026-10-30T05:00:00"))
    assert [m["id"] for m in got] == ["2", "3"]


def test_a_full_page_asks_for_the_next_one_before_its_oldest_message(monkeypatch):
    first = [msg(str(200 - i), "2026-10-29T12:00:00+00:00") for i in range(100)]
    second = [msg("1", "2026-10-29T06:00:00+00:00")]
    calls = serve(monkeypatch, {"/channels/t1/messages": lambda p: second if p.get("before") else first})
    got = dr._messages("t1", utc("2026-10-29T05:00:00"), utc("2026-10-30T05:00:00"))
    assert len(got) == 101 and got[0]["id"] == "1"
    assert calls[1][1]["before"] == "101"


def test_a_channel_the_bot_may_not_read_holds_no_messages(monkeypatch):
    serve(monkeypatch, {"/channels/t1/messages": forbidden()})
    assert dr._messages("t1", utc("2026-10-29T05:00:00"), utc("2026-10-30T05:00:00")) == []


def test_reading_the_server_leaves_out_the_summary_channel(monkeypatch):
    day = [msg("9", "2026-10-29T12:00:00+00:00")]
    calls = serve(monkeypatch, {
        f"/guilds/{dr.GUILD}/channels": GUILD_CHANNELS,
        f"/guilds/{dr.GUILD}/threads/active": {"threads": []},
        "/channels/t1/messages": day, "/channels/f1/messages": [],
        f"/channels/{dr.SUMMARY_CHANNEL}/messages": day,
        **{f"/channels/{c}/threads/archived/public": {"threads": [], "has_more": False} for c in ("t1", "f1", dr.SUMMARY_CHANNEL)},
    })
    out = dr.read_server("2026-10-29T05:00:00", "2026-10-30T05:00:00")
    assert "=== MoonLight / #moonlight-dev (1)" in out and "moonlight-daily" not in out
    assert (f"/channels/{dr.SUMMARY_CHANNEL}/messages", {"limit": 100}) not in calls   # its threads are read, its own posts are not


def test_a_message_line_carries_its_reply_its_attachment_and_its_link():
    m = msg("42", "2026-10-29T13:08:00+00:00", "yes",
            referenced_message={"content": "does it work?", "author": {"username": "bob", "global_name": "Bob"}},
            attachments=[{"filename": "shot.png", "url": "https://cdn.discordapp.com/x/shot.png"}])
    line = dr._line(m, "t1")
    assert line.startswith("[2026-10-29 13:08] anna (replying to Bob: 'does it work?'): yes")
    assert "attachment: shot.png https://cdn.discordapp.com/x/shot.png" in line
    assert line.endswith(f"<{dr.LINK}/t1/42>")


def test_an_attachment_is_fetched_only_from_discord():
    with pytest.raises(ValueError):
        dr.fetch_attachment("https://example.com/shot.png")


def test_an_attachment_announced_as_too_large_is_refused_before_a_byte_is_read(monkeypatch):
    class Big(io.BytesIO):
        headers = {"Content-Length": str(dr.MAX_ATTACHMENT + 1)}

        def read(self, n=-1):
            raise AssertionError("a byte was read")
    monkeypatch.setattr(dr.urllib.request, "urlopen", lambda req, timeout: Big())
    with pytest.raises(ValueError):
        dr.fetch_attachment("https://cdn.discordapp.com/attachments/1/2/big.mov")


def test_an_attachment_without_a_size_is_refused_once_it_streams_past_the_limit_and_leaves_nothing(monkeypatch, tmp_path):
    class Endless(io.BytesIO):
        headers = {}

        def read(self, n=-1):
            return b"x" * 8
    monkeypatch.setattr(dr, "MAX_ATTACHMENT", 20)
    monkeypatch.setattr(dr, "ATTACH_ROOT", tmp_path)
    monkeypatch.setattr(dr.urllib.request, "urlopen", lambda req, timeout: Endless())
    with pytest.raises(ValueError):
        dr.fetch_attachment("https://cdn.discordapp.com/attachments/1/2/log.txt")
    assert list(tmp_path.iterdir()) == []


def test_downloads_keep_only_the_newest_few_so_repeated_reads_never_fill_the_disk(monkeypatch, tmp_path):
    monkeypatch.setattr(dr, "ATTACH_ROOT", tmp_path)
    monkeypatch.setattr(dr, "KEEP_ATTACHMENTS", 3)
    monkeypatch.setattr(dr.urllib.request, "urlopen", lambda req, timeout: type("R", (io.BytesIO,), {"headers": {}})(b"png"))
    paths = [dr.fetch_attachment(f"https://cdn.discordapp.com/attachments/1/2/shot{i}.png") for i in range(5)]
    assert len(list(tmp_path.iterdir())) == 3
    assert Path(paths[-1]).read_bytes() == b"png"   # the newest stays readable


def test_a_limited_read_stops_paging_once_it_holds_enough(monkeypatch):
    pages = []

    def fake_get(path, **params):
        pages.append(params.get("before"))
        start = len(pages) * 100
        return [msg(str(10_000 - start - i), "2026-10-29T13:00:00+00:00", "hi") for i in range(100)]
    monkeypatch.setattr(dr, "_get", fake_get)
    got = dr._messages("c1", datetime(2026, 10, 28, tzinfo=timezone.utc), datetime(2026, 10, 30, tzinfo=timezone.utc), limit=150)
    assert len(got) == 150
    assert len(pages) == 2   # a third page would hold only messages older than the 150 asked for


def test_a_rate_limited_answer_waits_as_told_then_asks_again(monkeypatch):
    waits = []
    answers = iter([urllib.error.HTTPError("u", 429, "Too Many Requests", {"Retry-After": "2"}, io.BytesIO(b"<html>slow down</html>")),
                    io.BytesIO(b'{"ok": true}')])

    def fake_urlopen(req, timeout):
        a = next(answers)
        if isinstance(a, Exception):
            raise a
        return a
    monkeypatch.setattr(dr, "_token", lambda: "t")
    monkeypatch.setattr(dr.urllib.request, "urlopen", fake_urlopen)
    monkeypatch.setattr(dr.time, "sleep", waits.append)
    assert dr._get("/x") == {"ok": True}
    assert waits == [2.0]   # an HTML body falls back to the Retry-After header


def test_a_window_with_an_offset_is_converted_to_utc_and_one_without_is_read_as_utc():
    assert dr._utc("2026-10-29T07:00+02:00") == utc("2026-10-29T05:00:00")
    assert dr._utc("2026-10-29T05:00:00") == utc("2026-10-29T05:00:00")


def test_a_channel_is_found_by_the_start_of_its_name_and_an_empty_name_is_refused(monkeypatch):
    serve(monkeypatch, {f"/guilds/{dr.GUILD}/channels": GUILD_CHANNELS, f"/guilds/{dr.GUILD}/threads/active": {"threads": []}})
    assert dr._resolve("#moonlight-dev") == "t1"
    with pytest.raises(ValueError):
        dr._resolve("#")


def test_a_thread_archived_inside_the_window_is_read_and_one_archived_before_it_is_not(monkeypatch):
    def archived(cid):
        return {"threads": [{"id": "a1", "parent_id": cid, "name": "late", "thread_metadata": {"archive_timestamp": "2026-10-29T20:00:00+00:00"}},
                            {"id": "a0", "parent_id": cid, "name": "old", "thread_metadata": {"archive_timestamp": "2026-10-20T20:00:00+00:00"}}],
                "has_more": True}
    serve(monkeypatch, {f"/guilds/{dr.GUILD}/threads/active": {"threads": []}, "/channels/t1/threads/archived/public": archived("t1")})
    got = dr._threads({"t1": "#dev"}, utc("2026-10-29T05:00:00"))
    assert [t["id"] for t in got] == ["a1"]


def test_a_failure_other_than_a_refused_channel_is_raised_not_read_as_an_empty_channel(monkeypatch):
    serve(monkeypatch, {"/channels/t1/messages": urllib.error.HTTPError("u", 401, "Unauthorized", {}, io.BytesIO(b"{}"))})
    with pytest.raises(urllib.error.HTTPError):
        dr._messages("t1", utc("2026-10-29T05:00:00"), utc("2026-10-30T05:00:00"))


def test_a_limit_of_zero_reads_no_messages():
    assert dr.read_messages("anything", limit=0) == "no messages in that window"


def test_a_post_goes_to_the_agent_forum_and_pings_no_one(monkeypatch):
    posts = []
    monkeypatch.setattr(dr, "_post", lambda path, body: posts.append((path, body)) or {"id": "77"})
    link = dr.post_agent("Tempo harness", "hello")
    assert posts == [(f"/channels/{dr.AGENT_FORUM}/threads", {"name": "Tempo harness", "message": {"content": "hello", "allowed_mentions": {"parse": []}}})]
    assert link.endswith("/77")


def test_a_later_part_that_fails_names_the_started_post_so_the_rest_goes_on_there(monkeypatch):
    calls = []
    def post(path, body):
        calls.append(path)
        if len(calls) == 2:
            raise urllib.error.HTTPError("u", 500, "Server Error", {}, io.BytesIO(b"{}"))
        return {"id": "77"}
    monkeypatch.setattr(dr, "_post", post)
    with pytest.raises(RuntimeError, match="77"):
        dr.post_agent("Long", "\n\n".join(["a" * 1500] * 3))
    serve(monkeypatch, {"/channels/77": {"id": "77", "parent_id": dr.AGENT_FORUM}})
    assert dr.reply_agent("77", "the rest").endswith("/77/77")
    assert calls.count(f"/channels/{dr.AGENT_FORUM}/threads") == 1   # one post, resumed rather than started again


def test_a_reply_outside_the_agent_forum_is_refused_before_anything_is_sent(monkeypatch):
    serve(monkeypatch, {"/channels/5": {"id": "5", "parent_id": "1203994211301728296"}})
    monkeypatch.setattr(dr, "_post", lambda path, body: pytest.fail("posted outside the agent forum"))
    with pytest.raises(PermissionError):
        dr.reply_agent("5", "hi")


def test_a_reply_inside_the_agent_forum_is_sent_in_that_post(monkeypatch):
    serve(monkeypatch, {"/channels/9": {"id": "9", "parent_id": dr.AGENT_FORUM}})
    posts = []
    monkeypatch.setattr(dr, "_post", lambda path, body: posts.append(path) or {"id": "10"})
    assert dr.reply_agent("9", "hi").endswith("/9/10")
    assert posts == ["/channels/9/messages"]


def test_a_long_text_splits_at_paragraph_breaks_each_part_under_the_limit():
    text = "\n\n".join(["a" * 900] * 5)
    parts = dr._chunks(text)
    assert len(parts) == 3
    assert all(len(p) <= dr.MAX_MESSAGE for p in parts)
    assert "".join(p.replace("\n", "") for p in parts) == "a" * 4500
    assert dr._chunks("b" * 4100) == ["b" * 2000, "b" * 2000, "b" * 100]   # one paragraph past the limit splits hard


def test_a_post_carries_its_tags_by_name_and_an_unknown_tag_is_refused(monkeypatch):
    serve(monkeypatch, {f"/channels/{dr.AGENT_FORUM}": {"available_tags": [{"id": "1", "name": "question"}, {"id": "2", "name": "audio"}]}})
    posts = []
    monkeypatch.setattr(dr, "_post", lambda path, body: posts.append(body) or {"id": "3"})
    dr.post_agent("T", "x", ["question", "audio"])
    assert posts[0]["applied_tags"] == ["1", "2"]
    with pytest.raises(ValueError):
        dr.post_agent("T", "x", ["gossip"])


def test_a_reply_id_that_is_not_digits_is_refused_before_any_request(monkeypatch):
    monkeypatch.setattr(dr, "_get", lambda *a, **k: pytest.fail("asked Discord about a malformed id"))
    with pytest.raises(ValueError):
        dr.reply_agent("9/messages", "hi")
