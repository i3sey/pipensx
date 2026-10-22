#!/usr/bin/env python3
"""
Sync Telegram AutoRun Compatibility Topic (https://t.me/AutoRunn/275).
Uses the local Telethon session (~/.config/goose/telegram.session).
"""

from __future__ import annotations
import sys
import os
import json
import asyncio
from pathlib import Path

# Add path for goose config if needed
sys.path.insert(0, str(Path.home() / ".config" / "goose"))

try:
    import telegram_mcp
except ImportError:
    pass

from ai_extractor import extract_with_regex, extract_with_local_ai
from autorun_db import AutorunDB

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "resources" / "autorun" / "compatibility_db.json"
TOPIC_ID = 275
CHANNEL_USERNAME = "AutoRunn"


async def fetch_topic_messages(limit: int = 100):
    client = telegram_mcp.get_client()
    await client.connect()
    if not await client.is_user_authorized():
        print("Telegram client not authorized! Check ~/.config/goose/telegram.session")
        return []

    print(f"Connecting to @{CHANNEL_USERNAME} topic {TOPIC_ID}...")
    entity = await client.get_entity(CHANNEL_USERNAME)

    messages = []
    async for m in client.iter_messages(entity, reply_to=TOPIC_ID, limit=limit):
        if not m.text:
            continue
        sender = await m.get_sender()
        author = getattr(sender, "first_name", "") or ""
        if getattr(sender, "last_name", None):
            author += " " + sender.last_name

        messages.append({
            "id": m.id,
            "date": m.date.isoformat(),
            "author": author,
            "text": m.text
        })

    await client.disconnect()
    return messages


def sync_to_db(use_ai: bool = False, ai_url: str = "http://localhost:1234/v1/chat/completions"):
    db = AutorunDB(DB_PATH)
    print(f"Current DB has {len(db.records)} entries.")

    messages = asyncio.run(fetch_topic_messages())
    print(f"Fetched {len(messages)} messages from Telegram topic.")

    added = 0
    updated = 0
    for m in messages:
        txt = m["text"]
        if m["id"] in [275, 277, 3315, 3316]:
            continue

        if use_ai:
            parsed = extract_with_local_ai(txt)
        else:
            parsed = extract_with_regex(txt, mid=m["id"], author=m["author"], date=m["date"])

        if parsed and parsed.get("game_title"):
            title = parsed["game_title"]
            existing = db.get_by_title(title)
            if existing:
                updated += 1
            else:
                added += 1
            db.add_or_update(parsed)

    db.save()
    print(f"Sync complete! Added: {added}, Updated: {updated}. Total records: {len(db.records)}")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Sync AutoRun compatibility tests from Telegram")
    parser.add_argument("--ai", action="store_true", help="Use local AI (LM Studio/Ollama) for extraction")
    parser.add_argument("--ai-url", default="http://localhost:1234/v1/chat/completions", help="Local AI endpoint")
    args = parser.parse_args()

    sync_to_db(use_ai=args.ai, ai_url=args.ai_url)
