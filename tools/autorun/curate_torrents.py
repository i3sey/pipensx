#!/usr/bin/env python3
"""
Curate, inspect, verify, and export tested PC game torrents for AutoRun (Wine-NX).
Maps tested games from compatibility_db.json into curated_torrents.json
and exports them into the pipensx-catalog schema.
"""

from __future__ import annotations
import argparse
import html
import json
import logging
import os
import re
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

REPO_ROOT = Path(__file__).resolve().parents[2]
COMPAT_DB_PATH = REPO_ROOT / "resources" / "autorun" / "compatibility_db.json"
CURATED_PATH = REPO_ROOT / "resources" / "autorun" / "curated_torrents.json"

BANNED_EXECUTABLES = {
    "setup.exe", "install.exe", "autorun.exe", "unins000.exe", "uninstall.exe"
}
HEX_40_RE = re.compile(r"^[0-9a-fA-F]{40}$")


def load_json(path: Path) -> Any:
    if not path.exists():
        return []
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def save_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.write("\n")


def verify_entry(entry: Dict[str, Any]) -> List[str]:
    """Verify an autorun catalog entry against strict portable and schema rules."""
    errors = []
    title = entry.get("title", "")
    if not title:
        errors.append("Missing title")

    if entry.get("type") != "autorun":
        errors.append(f"Invalid type: '{entry.get('type')}', expected 'autorun'")

    if entry.get("category") != "PC Ports":
        errors.append(f"Invalid category: '{entry.get('category')}', expected 'PC Ports'")

    info_hash = entry.get("info_hash", "").strip()
    if not HEX_40_RE.match(info_hash):
        errors.append(f"Invalid info_hash: '{info_hash}', must be 40-char hex")

    magnet = entry.get("magnet", "")
    if not magnet.startswith("magnet:?xt=urn:btih:"):
        errors.append("Invalid magnet URI: must start with 'magnet:?xt=urn:btih:'")
    elif info_hash.lower() not in magnet.lower():
        errors.append(f"Magnet URI does not contain info_hash {info_hash}")

    cover = entry.get("cover", "")
    if not (cover.startswith("http://") or cover.startswith("https://")):
        errors.append(f"Invalid cover URL: '{cover}'")

    size_bytes = entry.get("size_bytes") or entry.get("size")
    if not isinstance(size_bytes, int) or size_bytes <= 0:
        errors.append(f"Invalid size_bytes: {size_bytes}")

    autorun = entry.get("autorun")
    if not isinstance(autorun, dict):
        errors.append("Missing 'autorun' metadata object")
        return errors

    exe = autorun.get("executable", "").strip()
    if not exe:
        errors.append("Missing autorun.executable")
    else:
        exe_lower = exe.lower()
        if not exe_lower.endswith(".exe"):
            errors.append(f"autorun.executable '{exe}' must end with .exe")
        exe_basename = Path(exe).name.lower()
        if exe_basename in BANNED_EXECUTABLES:
            errors.append(f"autorun.executable '{exe}' is an installer, not the game executable")
        if ".iso" in exe_lower or ".cue" in exe_lower:
            errors.append(f"autorun.executable '{exe}' points to a disk image")

    folder = autorun.get("target_folder", "").strip()
    if not folder or "/" in folder or "\\" in folder or ".." in folder:
        errors.append(f"Invalid autorun.target_folder: '{folder}'")

    valid_statuses = {"playable", "playable_with_tweaks", "unplayable", "issues"}
    status = autorun.get("status")
    if status not in valid_statuses:
        errors.append(f"Invalid autorun.status '{status}', must be one of {valid_statuses}")

    for req_field in ("fps", "overclock", "translator", "renderer"):
        if not autorun.get(req_field):
            errors.append(f"Missing autorun.{req_field}")

    return errors


def cmd_verify(args: argparse.Namespace) -> int:
    curated = load_json(CURATED_PATH)
    if not curated:
        print(f"[!] No entries found in {CURATED_PATH}")
        return 1

    print(f"[*] Verifying {len(curated)} curated AutoRun torrent entries...")
    all_ok = True
    for i, entry in enumerate(curated, 1):
        errs = verify_entry(entry)
        title = entry.get("title", f"Entry #{i}")
        if errs:
            all_ok = False
            print(f"  [FAIL] {i}. {title}:")
            for e in errs:
                print(f"         - {e}")
        else:
            exe = entry["autorun"]["executable"]
            fps = entry["autorun"]["fps"]
            folder = entry["autorun"]["target_folder"]
            reg = " [reg tweak]" if entry["autorun"].get("registry") else ""
            print(f"  [OK]   {i:2d}. {title:<32} -> {folder}/{exe:<20} ({fps} FPS){reg}")

    if all_ok:
        print(f"\n[+] SUCCESS: All {len(curated)} entries passed strict portable & schema verification!")
        return 0
    else:
        print("\n[!] Verification failed with errors.")
        return 1


def cmd_status(args: argparse.Namespace) -> int:
    compat = load_json(COMPAT_DB_PATH)
    curated = load_json(CURATED_PATH)

    print("=" * 80)
    print(f" AutoRun Curated Torrents Status")
    print("=" * 80)
    print(f"Total compatibility tests:  {len(compat)}")
    print(f"Curated ready-to-play:     {len(curated)}")
    print("-" * 80)
    print(f"{'#':<3} {'Title':<30} {'Status':<12} {'FPS':<10} {'Size':<10} {'Executable'}")
    print("-" * 80)

    for i, e in enumerate(curated, 1):
        ar = e.get("autorun", {})
        title = e.get("title", "")[:29]
        status = ar.get("status", "")[:11]
        fps = ar.get("fps", "")[:9]
        size_gb = (e.get("size_bytes", 0) / (1024 ** 3))
        size_str = f"{size_gb:.1f} GB" if size_gb >= 1.0 else f"{e.get('size_bytes', 0) / (1024 ** 2):.0f} MB"
        exe = f"{ar.get('target_folder')}/{ar.get('executable')}"
        print(f"{i:<3} {title:<30} {status:<12} {fps:<10} {size_str:<10} {exe}")

    print("=" * 80)
    return 0


def cmd_sync_catalog(args: argparse.Namespace) -> int:
    """Sync curated entries into the pipensx-catalog repository."""
    catalog_repo = Path(args.catalog_dir).resolve()
    if not catalog_repo.exists():
        print(f"[!] pipensx-catalog directory not found at: {catalog_repo}")
        return 1

    curated = load_json(CURATED_PATH)
    if not curated:
        print("[!] No curated entries to sync.")
        return 1

    # Verify first
    for entry in curated:
        errs = verify_entry(entry)
        if errs:
            print(f"[!] Validation error in '{entry.get('title')}': {errs}")
            return 1

    sys.path.insert(0, str(catalog_repo))
    from storage import Storage
    from export_catalog import export_catalog

    db_path = catalog_repo / "catalog.db"
    print(f"[*] Connecting to catalog SQLite DB: {db_path}")
    storage = Storage(db_path)

    # Insert or update each curated entry
    for entry in curated:
        topic_id = entry.get("topic_id")
        if not topic_id:
            # Generate deterministic synthetic topic_id if missing
            topic_id = int(entry["info_hash"][:7], 16)
            entry["topic_id"] = topic_id

        url = entry.get("url") or f"https://rutracker.org/forum/viewtopic.php?t={topic_id}"
        storage.upsert_topic(topic_id, url)
        storage.save_catalog_entry(
            topic_id=topic_id,
            title_id=None,
            entry_dict=entry
        )
        print(f"  [+] Saved to catalog.db: {topic_id} -> {entry['title']}")

    # Re-export catalog.json and manifest.json
    print(f"[*] Re-exporting catalog artifacts in {catalog_repo}...")
    export_catalog(
        storage=storage,
        output_dir=catalog_repo,
        overrides_path=catalog_repo / "overrides.json",
        schema_path=catalog_repo / "schema.json",
        catalog_commit="autorun-phase-3",
        source_commit="autorun-phase-3"
    )
    print(f"[+] Re-export successful! Check {catalog_repo / 'catalog.json'}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="AutoRun Curated Torrents Manager")
    subparsers = parser.add_subparsers(dest="command")

    p_verify = subparsers.add_parser("verify", help="Verify all curated torrent entries")
    p_status = subparsers.add_parser("status", help="Show overview of curated torrents")
    p_sync = subparsers.add_parser("sync-catalog", help="Sync curated torrents into pipensx-catalog repo")
    p_sync.add_argument("--catalog-dir", default=str(Path.home() / "pipensx-catalog"),
                        help="Path to pipensx-catalog repo")

    args = parser.parse_args()
    if args.command == "verify" or not args.command:
        return cmd_verify(args)
    elif args.command == "status":
        return cmd_status(args)
    elif args.command == "sync-catalog":
        return cmd_sync_catalog(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
