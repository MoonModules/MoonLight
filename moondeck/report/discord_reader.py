#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["mcp>=2"]
# ///
"""Read the MoonModules Discord server for an agent, as an MCP server: channels, threads and their messages through a bot's REST API, read-only.

The bot token lives in the macOS keychain (service `discord-reader`), read per request and never written to a file:
  security add-generic-password -s discord-reader -a bot -w
Registered once per machine, after which an agent session has the tools:
  claude mcp add --scope user discord -- uv run --script <repo>/moondeck/report/discord_reader.py
"""
from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone
from pathlib import Path

GUILD = "700041398778331156"
API = "https://discord.com/api/v10"
LINK = f"https://discord.com/channels/{GUILD}"
# Discord's documented bot User-Agent; its edge refuses the library default.
AGENT = "DiscordBot (https://github.com/MoonModules/MoonLight, 1)"
# Text, announcement and forum channels; the bot reads every one its role can see.
TEXT_TYPES = (0, 5, 15)
# Bot feeds that mirror GitHub: the issues and pull requests themselves are the source.
SKIP_CATEGORIES = ("GitHub Activity",)
# A picture or a log, not a video: the size an agent reads, streamed to disk and refused beyond it.
MAX_ATTACHMENT = 25 * 2**20
# Downloads live under one folder that keeps the newest few, so repeated reads never fill the disk while a returned path stays readable.
ATTACH_ROOT = Path(tempfile.gettempdir()) / "moonlight-discord"
KEEP_ATTACHMENTS = 20
# The daily summary, read on its own to find where the last one stopped, never summarized again.
SUMMARY_CHANNEL = "1565668213759676426"


def _token() -> str:
    try:
        return subprocess.run(["security", "find-generic-password", "-s", "discord-reader", "-w"],
                              capture_output=True, text=True, check=True).stdout.strip()
    except subprocess.CalledProcessError:
        raise RuntimeError("no bot token in the keychain: security add-generic-password -s discord-reader -a bot -w") from None


def _get(path: str, **params) -> object:
    url = API + path + ("?" + urllib.parse.urlencode(params) if params else "")
    req = urllib.request.Request(url, headers={"Authorization": f"Bot {_token()}", "User-Agent": AGENT})
    while True:
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code != 429:
                raise
            time.sleep(_retry_after(e))   # rate limited: wait as told, then again


def _retry_after(e: urllib.error.HTTPError) -> float:
    """How long a 429 asks to wait: Discord's JSON body, else the Retry-After header, else a second, since an edge's 429 can be HTML."""
    try:
        return float(json.load(e)["retry_after"])
    except (ValueError, KeyError, TypeError):
        try:
            return float(e.headers.get("Retry-After", 1))
        except (TypeError, ValueError):
            return 1.0


def _utc(iso: str) -> datetime:
    """An ISO time as UTC: one without an offset is read as UTC, one with an offset is converted."""
    t = datetime.fromisoformat(iso)
    return t.replace(tzinfo=timezone.utc) if t.tzinfo is None else t.astimezone(timezone.utc)


def _channels() -> dict[str, str]:
    """The server's text channels by id, named with their category, minus the GitHub feeds."""
    chs = _get(f"/guilds/{GUILD}/channels")
    cats = {c["id"]: c["name"] for c in chs if c["type"] == 4}
    out = {}
    for c in sorted(chs, key=lambda c: c["position"]):
        cat = cats.get(c.get("parent_id"), "")
        if c["type"] in TEXT_TYPES and cat not in SKIP_CATEGORIES:
            out[c["id"]] = f"{cat} / #{c['name']}" if cat else f"#{c['name']}"
    return out


def _threads(channels: dict[str, str], since: datetime | None = None) -> list[dict]:
    """The active threads of those channels, and with `since` the public threads archived after it, whose messages a window may still hold."""
    out = [t for t in _get(f"/guilds/{GUILD}/threads/active")["threads"] if t.get("parent_id") in channels]
    if since is None:
        return out
    seen = {t["id"] for t in out}
    for cid in channels:
        before = None
        while True:   # newest archived first, paged by archive time
            try:
                page = _get(f"/channels/{cid}/threads/archived/public", limit=100, **({"before": before} if before else {}))
            except urllib.error.HTTPError as e:
                if e.code in (403, 404):
                    break   # a channel the bot may not read, or a kind without threads
                raise
            fresh = [t for t in page.get("threads", []) if datetime.fromisoformat(t["thread_metadata"]["archive_timestamp"]) >= since]
            out += [t for t in fresh if t["id"] not in seen]
            seen.update(t["id"] for t in fresh)
            if not page.get("has_more") or len(fresh) < len(page.get("threads", [])):
                break
            before = page["threads"][-1]["thread_metadata"]["archive_timestamp"]
    return out


def _resolve(channel: str) -> str:
    """A channel or active thread by id, or by the start of its name, with or without '#'."""
    if channel.isdigit():
        if _get(f"/channels/{channel}").get("guild_id") == GUILD:
            return channel
        raise ValueError(f"{channel} is not on the server")
    want = channel.lstrip("#").lower()
    if not want:
        raise ValueError("name a channel or thread")
    chs = _channels()
    for cid, name in chs.items():
        if name.lower().split("#")[-1].startswith(want):
            return cid
    for t in _threads(chs):
        if t["name"].lower().startswith(want):
            return t["id"]
    raise ValueError(f"no channel or active thread named {channel!r}")


def _messages(cid: str, start: datetime, end: datetime, limit: int | None = None) -> list[dict]:
    """A channel's messages from start up to end, oldest first, the newest `limit` where one is given; none where the bot may not read."""
    got: list[dict] = []
    before = None
    while True:
        try:
            page = _get(f"/channels/{cid}/messages", limit=100, **({"before": before} if before else {}))
        except urllib.error.HTTPError as e:
            if e.code == 403:
                return []   # a channel the bot may not read; any other failure is an incomplete read, and says so
            raise
        for m in page:
            t = datetime.fromisoformat(m["timestamp"])
            if t < start:
                return list(reversed(got))
            if t < end:
                got.append(m)
                if limit is not None and len(got) >= limit:
                    return list(reversed(got))   # pages come newest first, so the rest of the window is older than what is asked
        if len(page) < 100:
            return list(reversed(got))
        before = page[-1]["id"]


def _line(m: dict, cid: str) -> str:
    """One message as a reader skims it: time, author, what it replies to, its attachments and its link."""
    who = m["author"].get("global_name") or m["author"]["username"]
    when = m["timestamp"][:16].replace("T", " ")
    out = f"[{when}] {who}: {m['content']}"
    if ref := m.get("referenced_message"):
        rwho = ref["author"].get("global_name") or ref["author"]["username"]
        out = f"[{when}] {who} (replying to {rwho}: {ref['content'][:80]!r}): {m['content']}"
    for a in m.get("attachments", []):
        out += f"\n    attachment: {a['filename']} {a['url']}"
    for e in m.get("embeds", []):
        if e.get("url"):
            out += f"\n    link: {e.get('title', '')} {e['url']}"
    if th := m.get("thread"):
        out += f"\n    thread: {th['name']} (id {th['id']})"
    return out + f"\n    <{LINK}/{cid}/{m['id']}>"


def list_channels() -> str:
    """The server's text channels and their active threads, with their ids."""
    chs = _channels()
    lines = [f"{name} (id {cid})" for cid, name in chs.items()]
    lines += [f"thread {t['name']} (id {t['id']}, in {chs[t['parent_id']]})" for t in _threads(chs)]
    return "\n".join(lines)


def read_messages(channel: str, hours: float = 24, limit: int = 300) -> str:
    """Messages of one channel or thread (id or name), oldest first, from the last `hours`, at most `limit`."""
    if limit <= 0:
        return "no messages in that window"
    cid = _resolve(channel)
    end = datetime.now(timezone.utc)
    got = _messages(cid, end - timedelta(hours=hours), end, limit)
    return "\n".join(_line(m, cid) for m in got) or "no messages in that window"


def read_server(start: str, end: str) -> str:
    """Every readable channel and thread's messages from `start` to `end` (ISO times, UTC unless they name an offset), grouped per channel, each with its link."""
    s, e = _utc(start), _utc(end)
    chs = _channels()
    places = [(cid, name) for cid, name in chs.items() if cid != SUMMARY_CHANNEL]
    places += [(t["id"], f"{chs[t['parent_id']]} > thread {t['name']}") for t in _threads(chs, s)]
    out = []
    for cid, name in places:
        got = _messages(cid, s, e)
        if got:
            out.append(f"=== {name} ({len(got)})")
            out += [_line(m, cid) for m in got]
    return "\n".join(out) or "no messages in that window"


def _prune_attachments() -> None:
    """Make room for one more download: the oldest go once the folder holds its share."""
    ATTACH_ROOT.mkdir(parents=True, exist_ok=True)
    held = sorted(ATTACH_ROOT.iterdir(), key=lambda p: p.stat().st_mtime)
    for old in held[:max(0, len(held) - (KEEP_ATTACHMENTS - 1))]:
        shutil.rmtree(old, ignore_errors=True)


def fetch_attachment(url: str) -> str:
    """Download a message's attachment (a screenshot, a log) to a temp file and return its path, for reading."""
    if not url.startswith(("https://cdn.discordapp.com/", "https://media.discordapp.net/")):
        raise ValueError("only Discord attachment URLs")
    req = urllib.request.Request(url, headers={"User-Agent": AGENT})
    too_large = ValueError(f"attachment larger than {MAX_ATTACHMENT // 2**20} MB")
    with urllib.request.urlopen(req, timeout=30) as r:
        # Refused on the announced size before a byte is read or a directory made; the count below covers a missing header.
        if int(r.headers.get("Content-Length") or 0) > MAX_ATTACHMENT:
            raise too_large
        _prune_attachments()
        path = Path(tempfile.mkdtemp(prefix="discord-", dir=ATTACH_ROOT)) / Path(url.split("?")[0]).name
        got = 0
        try:
            with path.open("wb") as f:
                while chunk := r.read(64 * 1024):
                    got += len(chunk)
                    if got > MAX_ATTACHMENT:
                        raise too_large
                    f.write(chunk)
        except BaseException:
            # A failed download leaves nothing behind, whatever failed it.
            path.unlink(missing_ok=True)
            path.parent.rmdir()
            raise
    return str(path)


def main() -> None:
    # Imported here, so the tests read the functions above without the MCP SDK installed.
    from mcp.server.mcpserver import MCPServer
    server = MCPServer("discord")
    for tool in (list_channels, read_messages, read_server, fetch_attachment):
        server.tool()(tool)
    server.run()


if __name__ == "__main__":
    main()
