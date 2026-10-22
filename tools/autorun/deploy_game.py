#!/usr/bin/env python3
"""
Deploy a tested PC game from PC directly into the Switch AutoRun directory.
Prepares folders, copies game assets, writes dxvk.conf, and merges user.reg tweaks.
"""

from __future__ import annotations
import sys
import os
import shutil
import argparse
from pathlib import Path
from typing import Optional, Dict, Any

from autorun_db import AutorunDB

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "resources" / "autorun" / "compatibility_db.json"

# Built-in registry tweaks and known game configs
GAME_REGISTRY_PRESETS = {
    "deus ex": """
[Software\\\\Eidos\\\\Deus Ex: HR]
"Aspect"="16:9"
"Fullscreen"=dword:00000001
"ResolutionWidth"=dword:000003c0
"ResolutionHeight"=dword:0000021c
"AspectRatio"=dword:00000002
"VSync"=dword:00000000
""",
    "barnyard": """
[Software\\\\THQ\\\\Barnyard]
"EnableGamepad"=dword:00000001
""",
    "flatout": """
[Software\\\\Bugbear\\\\FlatOut2]
"DisplayWidth"=dword:00000500
"DisplayHeight"=dword:000002d0
"Fullscreen"=dword:00000001
"""
}

# Default recommended DXVK config for Switch
DEFAULT_DXVK_CONF = """# AutoRun Switch DXVK optimized configuration
dxvk.enableAsync = true
dxvk.numCompilerThreads = 4
dxvk.maxChunkSize = 32
d3d9.maxFrameLatency = 1
dxgi.maxFrameLatency = 1
"""


def find_registry_tweak(title: str) -> Optional[str]:
    lower = title.lower()
    for key, tweak in GAME_REGISTRY_PRESETS.items():
        if key in lower:
            return tweak
    return None


def merge_user_reg(reg_path: Path, tweak_content: str):
    """Safely merges a registry block into user.reg."""
    if not reg_path.exists():
        reg_path.parent.mkdir(parents=True, exist_ok=True)
        with open(reg_path, "w", encoding="utf-8") as f:
            f.write("WINE REGISTRY Version 2\n;; All keys relative to \\\\User\n\n")
            f.write(tweak_content.strip() + "\n")
        print(f"  [+] Created user.reg with game tweak at: {reg_path}")
        return

    with open(reg_path, "r", encoding="utf-8", errors="ignore") as f:
        existing = f.read()

    # Check if header already in user.reg
    first_header = [l.strip() for l in tweak_content.strip().split('\n') if l.strip().startswith('[')][0]
    if first_header in existing:
        print(f"  [*] Registry section {first_header} already present in user.reg (skipping merge).")
        return

    with open(reg_path, "a", encoding="utf-8") as f:
        f.write("\n" + tweak_content.strip() + "\n")
    print(f"  [+] Successfully merged registry tweak into: {reg_path}")


def deploy(game_query: str, src_path: Path, sd_root: Path, dry_run: bool = False):
    db = AutorunDB(DB_PATH)
    record = db.get_by_title(game_query)

    if not record:
        print(f"[!] Warning: '{game_query}' not found in compatibility DB. Using generic AutoRun profile.")
        title = game_query
        badge = "⚪"
        notes = "No prior community tests"
        oc = "Safe Max (CPU 1785, GPU 614, RAM 1900)"
        translator = "Box64 / FEX"
    else:
        title = record["game_title"]
        badge = record["status_badge"]
        notes = record["notes_and_fixes"]
        oc = record["overclock"]
        translator = record["cpu_translator"]

    print("=" * 65)
    print(f"Deploying {badge} {title} to Nintendo Switch AutoRun")
    print("=" * 65)
    print(f"  Source PC Path:      {src_path}")
    print(f"  SD Root:             {sd_root}")
    print(f"  Recommended OC:      {oc}")
    print(f"  CPU Translator:      {translator}")
    print(f"  Test Notes / Fixes:  {notes}")
    print("-" * 65)

    if not src_path.exists():
        print(f"[ERROR] Source folder does not exist: {src_path}", file=sys.stderr)
        sys.exit(1)

    # Sanitize folder name
    safe_folder = "".join([c if c.isalnum() or c in " ._-" else "_" for c in title]).strip()
    dest_game_dir = sd_root / "switch" / "wine" / "drive_c" / "Games" / safe_folder
    user_reg_path = sd_root / "switch" / "wine" / "registry" / "user.reg"

    print(f"  Target game directory: {dest_game_dir}")

    if dry_run:
        print("\n[DRY RUN] No files were copied.")
        return

    # 1. Copy files
    print("\n[1/3] Copying game files to Switch SD...")
    dest_game_dir.mkdir(parents=True, exist_ok=True)
    if src_path.is_dir():
        # Copy tree with progress
        for item in src_path.iterdir():
            target_item = dest_game_dir / item.name
            if item.is_dir():
                if not target_item.exists():
                    shutil.copytree(item, target_item)
            else:
                shutil.copy2(item, target_item)
    else:
        shutil.copy2(src_path, dest_game_dir)
    print("  [+] Game files copied.")

    # 2. Write optimized dxvk.conf
    print("\n[2/3] Writing optimized dxvk.conf...")
    dxvk_file = dest_game_dir / "dxvk.conf"
    with open(dxvk_file, "w", encoding="utf-8") as f:
        f.write(DEFAULT_DXVK_CONF)
    print(f"  [+] dxvk.conf created at: {dxvk_file}")

    # 3. Apply registry tweaks if known
    print("\n[3/3] Checking registry tweaks...")
    tweak = find_registry_tweak(title)
    if tweak:
        print(f"  [!] Found registry tweak preset for {title}")
        merge_user_reg(user_reg_path, tweak)
    else:
        print("  [*] No specific registry tweaks required for this game.")

    print("\n" + "=" * 65)
    print("✨ Deployment complete! Game is ready to launch in AutoRun.")
    print(f"To launch: open hbmenu -> AutoRun -> navigate to drive_c/Games/{safe_folder}")
    print(f"Recommended UltraNX profile: {oc}")
    print("=" * 65)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Deploy PC game to Nintendo Switch AutoRun with optimal configs")
    parser.add_argument("--game", required=True, help="Game title to search in compatibility DB (e.g. 'Deus Ex' or 'FlatOut 2')")
    parser.add_argument("--src", required=True, type=Path, help="Path to game folder on PC")
    parser.add_argument("--sd-root", required=True, type=Path, help="Path to Switch SD card mount root (e.g. /run/media/i3sey/SWITCH or ./staging)")
    parser.add_argument("--dry-run", action="store_true", help="Preview deploy plan without copying files")

    args = parser.parse_args()
    deploy(args.game, args.src, args.sd_root, args.dry_run)
