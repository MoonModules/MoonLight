"""The Discord reader: which channels it reads, which messages a window holds, and how a message reads. Run: `uv run moondeck/test/test_host.py --python`."""

import io
import urllib.error
from datetime import datetime, timezone

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
    })
    out = dr.read_server("2026-10-29T05:00:00", "2026-10-30T05:00:00")
    assert "=== MoonLight / #moonlight-dev (1)" in out and "moonlight-daily" not in out
    assert all(dr.SUMMARY_CHANNEL not in path for path, _ in calls)


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
