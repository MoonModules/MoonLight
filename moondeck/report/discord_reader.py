#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["mcp>=2"]
# ///
"""Read the MoonModules Discord server for an agent, as an MCP server: channels, threads and their messages through a bot's REST API, and post in the one forum agents talk to each other in.

The bot token lives in the macOS keychain (service `discord-reader`, account `bot`), read per request and kept there alone. Storing the token, registering this file as an MCP server, calling the REST API without one, and the daily summary's format: moondeck/MoonDeck.md#discord_reader.
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
# The forum agents write in directly, and the one place the bot posts: every other channel stays read-only.
AGENT_FORUM = "1567531890498609192"
# Discord's longest message, which a longer text splits under at its paragraph breaks.
MAX_MESSAGE = 2000


def _token() -> str:
    try:
        return subprocess.run(["security", "find-generic-password", "-s", "discord-reader", "-w"],
                              capture_output=True, text=True, check=True).stdout.strip()
    except subprocess.CalledProcessError:
        raise RuntimeError("no bot token in the keychain: security add-generic-password -s discord-reader -a bot -w") from None


def _send(req: urllib.request.Request) -> object:
    """Send a request with the bot's token and answer its JSON, waiting out a rate limit as told, then asking again."""
    req.add_header("Authorization", f"Bot {_token()}")
    req.add_header("User-Agent", AGENT)
    while True:
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            if e.code != 429:
                raise
            time.sleep(_retry_after(e))


def _get(path: str, **params) -> object:
    return _send(urllib.request.Request(API + path + ("?" + urllib.parse.urlencode(params) if params else "")))


def _post(path: str, body: dict) -> object:
    """POST a JSON body."""
    return _send(urllib.request.Request(API + path, data=json.dumps(body).encode(), method="POST",
                                        headers={"Content-Type": "application/json"}))


def _chunks(text: str) -> list[str]:
    """A text as messages Discord takes, split at paragraph breaks, then at line breaks, then hard, each under MAX_MESSAGE."""
    out, cur = [], ""
    for para in text.split("\n\n"):
        piece = para if not cur else cur + "\n\n" + para
        if len(piece) <= MAX_MESSAGE:
            cur = piece
            continue
        if cur:
            out.append(cur)
        while len(para) > MAX_MESSAGE:
            cut = para.rfind("\n", 0, MAX_MESSAGE)
            cut = cut if cut > 0 else MAX_MESSAGE
            out.append(para[:cut])
            para = para[cut:].lstrip("\n")
        cur = para
    if cur:
        out.append(cur)
    return out


# A post names no one by accident: a mention in an agent's text never pings a person.
NO_PINGS = {"parse": []}


def post_agent(title: str, text: str, tags: list[str] | None = None) -> str:
    """Start a post in the agent-to-agent forum with this title, text and tags by name (question, proposal, measurement, review and the topics the forum lists); answers its link. A later part that fails raises with the post's id, so the rest goes on with reply_agent rather than a second post."""
    if not title.strip() or not text.strip():
        raise ValueError("a post needs a title and a text")
    body = {"name": title[:100], "message": {"content": "", "allowed_mentions": NO_PINGS}}
    if tags:
        known = {t["name"]: t["id"] for t in _get(f"/channels/{AGENT_FORUM}").get("available_tags", [])}
        unknown = [t for t in tags if t not in known]
        if unknown:
            raise ValueError(f"no such tag in the forum: {', '.join(unknown)}; it has {', '.join(known)}")
        body["applied_tags"] = [known[t] for t in tags]
    parts = _chunks(text)
    body["message"]["content"] = parts[0]
    thread = _post(f"/channels/{AGENT_FORUM}/threads", body)
    for sent, part in enumerate(parts[1:], start=1):
        try:
            _post(f"/channels/{thread['id']}/messages", {"content": part, "allowed_mentions": NO_PINGS})
        except Exception as e:
            raise RuntimeError(f"post {thread['id']} started, {sent} of {len(parts)} parts sent; "
                               f"send the rest with reply_agent({thread['id']!r}, ...): {e}") from e
    return f"{LINK}/{thread['id']}"


def reply_agent(thread: str, text: str) -> str:
    """Reply in one post of the agent-to-agent forum, by the post's id; any other channel is refused. Answers the reply's link."""
    if not text.strip():
        raise ValueError("a reply needs a text")
    if not str(thread).isdigit():
        raise ValueError("a post's id is digits")
    if str(_get(f"/channels/{thread}").get("parent_id")) != AGENT_FORUM:
        raise PermissionError("the bot posts only in the agent-to-agent forum")
    last = None
    for part in _chunks(text):
        last = _post(f"/channels/{thread}/messages", {"content": part, "allowed_mentions": NO_PINGS})
    return f"{LINK}/{thread}/{last['id']}"


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
    for tool in (list_channels, read_messages, read_server, fetch_attachment, post_agent, reply_agent):
        server.tool()(tool)
    server.run()


if __name__ == "__main__":
    main()
