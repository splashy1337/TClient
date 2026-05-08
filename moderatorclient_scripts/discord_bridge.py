#!/usr/bin/env python3
"""
Moderator Client Discord bridge.

Polls the KoG to-do forum channel and writes todo_players.json that Moderator Client
reads. A "to-do" post is one where the post has the 'To-Do' tag applied OR the
starter message has no :white_check_mark: reaction (KoG mods put a ✅ on resolved
posts; the tag is unreliable because they forget to switch it to 'Done').

WARNING
-------
This script uses a Discord USER TOKEN (selfbot), which violates Discord's Terms
of Service. Your account can be banned for using it. Use a throwaway account
if you care about the main one. You explicitly acknowledged this risk.

Setup
-----
1. Install the selfbot fork of discord.py:

       pip install -U discord.py-self

2. Put your Discord user token in ONE of these places:

   - Environment variable:  MODERATORCLIENT_DISCORD_TOKEN=<token>
   - File (preferred):      %APPDATA%/DDNet/discord_token.txt

3. Run the bridge:

       python discord_bridge.py --channel-id <your_forum_channel_id>

   Keep the terminal open. The script polls every 60 seconds and writes
   %APPDATA%/DDNet/todo_players.json atomically on every successful poll.
   A log is written to %APPDATA%/DDNet/discord_bridge.log.
"""

import argparse
import asyncio
import json
import os
import sys
import tempfile
import traceback
from datetime import datetime, timedelta, timezone
from pathlib import Path

import discord

TODO_TAG_NAME = "To-Do"
DONE_TAG_NAME = "Done"
CHECKMARK = "✅"
POLL_INTERVAL_SECONDS = 60
RECONNECT_DELAY_SECONDS = 30


def config_dir() -> Path:
    # Matches DDNet/TClient/Moderator Client storage path (all share %APPDATA%/DDNet).
    appdata = os.environ.get("APPDATA")
    if appdata:
        return Path(appdata) / "DDNet"
    return Path.home() / ".config" / "DDNet"


# --- logging ------------------------------------------------------------------

_log_file = None


def _log(msg: str, *, error: bool = False) -> None:
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {msg}"
    stream = sys.stderr if error else sys.stdout
    try:
        print(line, file=stream, flush=True)
    except Exception:
        pass
    if _log_file is not None:
        try:
            print(line, file=_log_file, flush=True)
        except Exception:
            pass


def setup_logging() -> None:
    global _log_file
    log_path = config_dir() / "discord_bridge.log"
    config_dir().mkdir(parents=True, exist_ok=True)
    try:
        _log_file = open(log_path, "a", encoding="utf-8", buffering=1)
        _log(f"--- bridge starting (pid={os.getpid()}) ---")
    except Exception as e:
        print(f"[bridge] could not open log file {log_path}: {e}", file=sys.stderr)


# ------------------------------------------------------------------------------


def load_token() -> str:
    env = os.environ.get("MODERATORCLIENT_DISCORD_TOKEN")
    if env and env.strip():
        return env.strip()
    token_file = config_dir() / "discord_token.txt"
    if token_file.exists():
        tok = token_file.read_text(encoding="utf-8").strip()
        if tok:
            return tok
    raise SystemExit(
        f"No Discord token found.\n"
        f"Set MODERATORCLIENT_DISCORD_TOKEN or write token to {token_file}"
    )


def atomic_write_json(path: Path, data) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(dir=str(path.parent), suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
        os.replace(tmp_path, path)
    except Exception:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass
        raise


def has_checkmark(message) -> bool:
    for r in getattr(message, "reactions", None) or []:
        if str(r.emoji) == CHECKMARK:
            return True
    return False


async def fetch_starter(thread) -> "discord.Message | None":
    # In a ForumChannel thread the starter message has id == thread.id.
    try:
        return await thread.fetch_message(thread.id)
    except Exception:
        pass
    try:
        async for m in thread.history(limit=1, oldest_first=True):
            return m
    except Exception:
        return None
    return None


async def collect_todos(client: discord.Client, channel_id: int) -> list:
    channel = client.get_channel(channel_id)
    if channel is None:
        channel = await client.fetch_channel(channel_id)

    cutoff = datetime.now(timezone.utc) - timedelta(days=90)

    threads = list(getattr(channel, "threads", []) or [])

    # Also pull archived threads; archived_threads() sorts by archive_timestamp
    # descending so we can stop early once we pass the 3-month cutoff.
    try:
        async for t in channel.archived_threads(limit=None):
            archive_ts = getattr(t, "archive_timestamp", None)
            if archive_ts is not None and archive_ts < cutoff:
                break
            threads.append(t)
    except Exception as e:
        _log(f"could not fetch archived threads: {e}", error=True)

    # Deduplicate (active thread list and archived list can overlap).
    seen: set = set()
    unique: list = []
    for t in threads:
        if t.id not in seen:
            seen.add(t.id)
            unique.append(t)

    results = []
    for thread in unique:
        try:
            tag_names = {t.name for t in getattr(thread, "applied_tags", None) or []}
            starter = await fetch_starter(thread)
            if starter is None:
                continue

            has_check = has_checkmark(starter)
            has_done_tag = DONE_TAG_NAME in tag_names

            # Exclude if resolved: Done tag or checkmark (or both).
            if has_done_tag or has_check:
                continue

            # Collect attachments from the first 10 messages (starter + early replies).
            seen_urls: set = set()
            attachments = []
            async for msg in thread.history(limit=10, oldest_first=True):
                for a in getattr(msg, "attachments", None) or []:
                    if a.url not in seen_urls:
                        seen_urls.add(a.url)
                        attachments.append({"url": a.url, "filename": a.filename})
                # Also capture images from embeds (e.g. re-hosted or linked media).
                for embed in getattr(msg, "embeds", None) or []:
                    for img in (embed.image, embed.thumbnail, embed.video):
                        if img is None:
                            continue
                        url = str(img.url) if img.url else ""
                        if url and url not in seen_urls:
                            seen_urls.add(url)
                            fname = url.split("/")[-1].split("?")[0] or "embed_media"
                            attachments.append({"url": url, "filename": fname})

            guild_id = getattr(thread.guild, "id", None)
            first_msg_ts = str(int(starter.created_at.timestamp())) if starter.created_at else ""
            results.append({
                "name": thread.name,
                "reason": starter.content or "",
                "post_id": str(thread.id),
                "post_url": (
                    f"https://discord.com/channels/{guild_id}/{thread.id}"
                    if guild_id is not None else ""
                ),
                "first_message_at": first_msg_ts,
                "attachments": attachments,
            })
        except Exception as e:
            _log(f"error on thread {getattr(thread, 'id', '?')}: {e}", error=True)
    return results


async def process_done_commands(client: discord.Client) -> None:
    """Process mark_done command files written by the C++ client."""
    cfg = config_dir()
    for cmd_file in cfg.glob("todo_done_*.json"):
        try:
            data = json.loads(cmd_file.read_text(encoding="utf-8"))
            post_id_str = data.get("post_id", "").strip()
            if not post_id_str:
                continue
            thread_id = int(post_id_str)

            thread = client.get_channel(thread_id)
            if thread is None:
                thread = await client.fetch_channel(thread_id)

            starter = await fetch_starter(thread)
            if starter is not None:
                try:
                    await starter.add_reaction(CHECKMARK)
                except Exception as e:
                    _log(f"could not react on {thread_id}: {e}", error=True)
                try:
                    await starter.reply("Done")
                except Exception as e:
                    _log(f"could not reply to starter on {thread_id}: {e}", error=True)
            else:
                _log(f"could not find starter message for {thread_id}", error=True)

            _log(f"marked done: thread {thread_id}")
        except Exception as e:
            _log(f"error processing {cmd_file.name}: {e}", error=True)
        finally:
            try:
                cmd_file.unlink()
            except OSError:
                pass


async def _run_session(token: str, channel_id: int, out_path: Path) -> bool:
    """Run one Discord session. Returns True if the exit was fatal (don't retry)."""
    client = discord.Client()
    ready = asyncio.Event()

    @client.event
    async def on_ready():
        _log(f"logged in as {client.user}")
        ready.set()

    @client.event
    async def on_disconnect():
        _log("disconnected from Discord", error=True)

    # Use default-argument capture so each session's tasks reference the right client/ready.
    async def _poll(c=client, r=ready):
        await r.wait()
        while not c.is_closed():
            try:
                todos = await collect_todos(c, channel_id)
                atomic_write_json(out_path, {
                    "updated_at": datetime.now(timezone.utc).isoformat(),
                    "todos": todos,
                })
                _log(f"wrote {len(todos)} todos to {out_path}")
            except Exception as e:
                _log(f"poll error: {e}", error=True)
            await asyncio.sleep(POLL_INTERVAL_SECONDS)

    async def _cmds(c=client, r=ready):
        await r.wait()
        while not c.is_closed():
            try:
                await process_done_commands(c)
            except Exception as e:
                _log(f"done-command error: {e}", error=True)
            await asyncio.sleep(5)

    poll_task = asyncio.create_task(_poll())
    command_task = asyncio.create_task(_cmds())
    fatal = False
    try:
        await client.start(token)
    except discord.LoginFailure as e:
        _log(f"login failed (bad token?): {e}", error=True)
        fatal = True
    except asyncio.CancelledError:
        fatal = True  # KeyboardInterrupt path — don't reconnect
        raise
    except Exception as e:
        _log(f"session ended unexpectedly: {e}", error=True)
        traceback.print_exc(file=sys.stderr)
        if _log_file is not None:
            traceback.print_exc(file=_log_file)
    finally:
        for task in (poll_task, command_task):
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        if not client.is_closed():
            try:
                await client.close()
            except Exception:
                pass
    return fatal


async def main_async(channel_id: int):
    token = load_token()
    out_path = config_dir() / "todo_players.json"

    while True:
        fatal = await _run_session(token, channel_id, out_path)
        if fatal:
            break
        _log(f"reconnecting in {RECONNECT_DELAY_SECONDS}s...")
        await asyncio.sleep(RECONNECT_DELAY_SECONDS)


def main():
    setup_logging()
    parser = argparse.ArgumentParser(description="Moderator Client Discord bridge")
    parser.add_argument("--channel-id", type=int, required=True,
                        help="Discord forum channel ID to poll for moderator reports")
    args = parser.parse_args()
    try:
        asyncio.run(main_async(args.channel_id))
    except KeyboardInterrupt:
        _log("stopped by user")


if __name__ == "__main__":
    main()
