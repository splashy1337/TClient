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
   - File (preferred):      %APPDATA%/Moderator Client/discord_token.txt

3. Run the bridge:

       python discord_bridge.py

   Keep the terminal open. The script polls every 60 seconds and writes
   %APPDATA%/Moderator Client/todo_players.json atomically on every successful poll.
"""

import argparse
import asyncio
import json
import os
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

import discord

DEFAULT_FORUM_CHANNEL_ID = 1020457397812203540
TODO_TAG_NAME = "To-Do"
DONE_TAG_NAME = "Done"
CHECKMARK = "✅"
POLL_INTERVAL_SECONDS = 60


def config_dir() -> Path:
    # Matches DDNet/TClient/Moderator Client storage path (all share %APPDATA%/DDNet).
    appdata = os.environ.get("APPDATA")
    if appdata:
        return Path(appdata) / "DDNet"
    return Path.home() / ".config" / "DDNet"


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
        print(f"[bridge] could not fetch archived threads: {e}", file=sys.stderr)

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
            is_todo_tag = TODO_TAG_NAME in tag_names
            has_done_tag = DONE_TAG_NAME in tag_names

            # Exclude if resolved: Done tag or checkmark (or both).
            if has_done_tag or has_check:
                continue

            # Collect attachments from the first 10 messages (starter + early replies).
            # Keeping the limit small avoids rate-limiting with many open threads.
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
            print(f"[bridge] error on thread {getattr(thread, 'id', '?')}: {e}",
                  file=sys.stderr)
    return results


async def process_done_commands(client: discord.Client) -> None:
    """Process mark_done command files written by the C++ client.

    The client writes todo_done_<post_id>.json files.  For each one we:
      1. React with ✅ on the starter message of that thread.
      2. Reply "Done" to the thread.
      3. Delete the command file so it isn't processed twice.
    """
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
                    print(f"[bridge] could not react on {thread_id}: {e}", file=sys.stderr)
                try:
                    await starter.reply("Done")
                except Exception as e:
                    print(f"[bridge] could not reply to starter on {thread_id}: {e}", file=sys.stderr)
            else:
                print(f"[bridge] could not find starter message for {thread_id}", file=sys.stderr)

            print(f"[bridge] marked done: thread {thread_id}")
        except Exception as e:
            print(f"[bridge] error processing {cmd_file.name}: {e}", file=sys.stderr)
        finally:
            try:
                cmd_file.unlink()
            except OSError:
                pass


async def main_async(channel_id: int):
    token = load_token()
    out_path = config_dir() / "todo_players.json"
    client = discord.Client()

    ready = asyncio.Event()

    @client.event
    async def on_ready():
        print(f"[bridge] logged in as {client.user}")
        ready.set()

    async def command_loop():
        """Fast loop: checks for mark_done commands every 5 s, independent of the main poll."""
        await ready.wait()
        while not client.is_closed():
            try:
                await process_done_commands(client)
            except Exception as e:
                print(f"[bridge] done-command error: {e}", file=sys.stderr)
            await asyncio.sleep(5)

    async def poll_loop():
        await ready.wait()
        while not client.is_closed():
            try:
                todos = await collect_todos(client, channel_id)
                atomic_write_json(out_path, {
                    "updated_at": datetime.now(timezone.utc).isoformat(),
                    "todos": todos,
                })
                print(f"[bridge] wrote {len(todos)} todos to {out_path}")
            except Exception as e:
                print(f"[bridge] poll error: {e}", file=sys.stderr)
            await asyncio.sleep(POLL_INTERVAL_SECONDS)

    poll_task = asyncio.create_task(poll_loop())
    command_task = asyncio.create_task(command_loop())
    try:
        await client.start(token)
    finally:
        for task in (poll_task, command_task):
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass


def main():
    parser = argparse.ArgumentParser(description="Moderator Client Discord bridge")
    parser.add_argument("--channel-id", type=int, default=DEFAULT_FORUM_CHANNEL_ID,
                        help="Discord forum channel ID to poll for moderator reports")
    args = parser.parse_args()
    try:
        asyncio.run(main_async(args.channel_id))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
