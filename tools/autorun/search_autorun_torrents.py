#!/usr/bin/env python3
"""
Search RuTracker for real torrent releases for the 20 AutoRun PC games.
Uses Chromium session cookies via undetected_chromedriver.
"""

from __future__ import annotations
import os
import sys
import time
import json
import re
import urllib.parse
from pathlib import Path
from bs4 import BeautifulSoup
import undetected_chromedriver as uc
from pycookiecheat import chrome_cookies

REPO_ROOT = Path(__file__).resolve().parents[2]

GAMES = [
    ("FlatOut 2", ["FlatOut 2", "FlatOut 2 GOG"]),
    ("Deus Ex: Human Revolution", ["Deus Ex Human Revolution", "Deus Ex Human Revolution Director"]),
    ("Barnyard", ["Barnyard", "Рога и копыта"]),
    ("Dead Space", ["Dead Space 2008", "Dead Space"]),
    ("Condemned: Criminal Origins", ["Condemned Criminal Origins", "Condemned"]),
    ("Deus Ex: Game of the Year Edition", ["Deus Ex Game of the Year", "Deus Ex GOTY", "Deus Ex"]),
    ("Ultimate Spider-Man", ["Ultimate Spider-Man", "Ultimate Spider Man"]),
    ("Need For Speed: Most Wanted", ["Need For Speed Most Wanted 2005", "Need For Speed Most Wanted Black Edition", "Need For Speed Most Wanted"]),
    ("Need For Speed: Carbon", ["Need For Speed Carbon", "Need For Speed Carbon Collector"]),
    ("Need For Speed: Undercover", ["Need For Speed Undercover"]),
    ("Devil May Cry 4", ["Devil May Cry 4", "Devil May Cry 4 Special Edition"]),
    ("Silent Hill 2 Enhanced Edition", ["Silent Hill 2 Director", "Silent Hill 2 Enhanced", "Silent Hill 2"]),
    ("Faster Than Light (FTL)", ["Faster Than Light", "FTL Faster Than Light"]),
    ("Dead Space 2", ["Dead Space 2"]),
    ("Dead Space 3", ["Dead Space 3"]),
    ("X-Men Origins: Wolverine", ["X-Men Origins Wolverine", "X Men Origins Wolverine"]),
    ("Spider-Man: Web of Shadows", ["Spider-Man Web of Shadows", "Spider Man Web of Shadows"]),
    ("The Path", ["The Path Tale of Tales", "The Path"]),
    ("Grand Theft Auto IV", ["Grand Theft Auto IV", "GTA IV Complete", "GTA IV"]),
    ("Deadpool", ["Deadpool"]),
]

# RuTracker categories that are for PC Windows games
PC_CATEGORIES = {
    "горячие новинки",
    "шутеры от первого лица",
    "игры для windows",
    "аркады",
    "гонки",
    "rpg",
    "ролевые игры",
    "симуляторы",
    "квесты",
    "приключения",
    "стратегии",
    "экшены",
    "старые игры",
    "инди-игры",
    "шутеры",
    "action",
}

def is_pc_category(cat: str) -> bool:
    c = cat.lower()
    return any(p in c for p in PC_CATEGORIES) and not any(cons in c for cons in ["ps2", "ps3", "ps4", "psp", "xbox", "nintendo", "mac", "ios", "android", "саундтрек", "музыка", "фильм", "сериал", "книг", "журнал", "видео"])

def is_pc_title(title: str) -> bool:
    t = title.lower()
    banned = ["[nro]", "[nsp]", "[nsz]", "[xci]", "[ps2]", "[ps3]", "[ps4]", "[psp]", "[x360]", "[xbox]", "[wii]", "[gamecube]", "[switch]", "[android]", "[ios]", "[macos]", "ost", "soundtrack"]
    return not any(b in t for b in banned)

def parse_size_bytes(text: str) -> int:
    text = text.replace("&nbsp;", " ").replace("\xa0", " ").strip()
    m = re.match(r"^([\d.]+)\s*([KkMmGgTt]?[Bb]|[КкМмГгТт]?[Бб])$", text)
    if not m:
        digits = re.sub(r"[^\d]", "", text)
        return int(digits) if digits else 0
    val = float(m.group(1))
    unit = m.group(2).upper()
    multiplier = 1
    if "K" in unit or "К" in unit: multiplier = 1024
    elif "M" in unit or "М" in unit: multiplier = 1024 ** 2
    elif "G" in unit or "Г" in unit: multiplier = 1024 ** 3
    elif "T" in unit or "Т" in unit: multiplier = 1024 ** 4
    return int(val * multiplier)

def clean_topic_id(href: str) -> int:
    m = re.search(r"viewtopic\.php\?t=(\d+)", href)
    return int(m.group(1)) if m else 0

def main():
    cookies = chrome_cookies("https://rutracker.org", browser="chromium")
    options = uc.ChromeOptions()
    options.add_argument("--disable-gpu")
    options.add_argument("--disable-dev-shm-usage")
    options.add_argument("--no-first-run")

    print("[*] Launching Chromium...")
    driver = uc.Chrome(
        options=options,
        browser_executable_path="/usr/bin/chromium",
        version_main=153,
        headless=False,
        use_subprocess=True
    )

    driver.get("https://rutracker.org/forum/index.php")
    time.sleep(2)
    for name, val in cookies.items():
        driver.add_cookie({"name": name, "value": val, "domain": ".rutracker.org", "path": "/"})

    selected = {}
    for game_name, queries in GAMES:
        print(f"\n==========================================")
        print(f"[*] Processing: {game_name}")
        print(f"==========================================")
        chosen = None

        for q in queries:
            url = "https://rutracker.org/forum/tracker.php?nm=" + urllib.parse.quote(q) + "&o=10&s=2"
            print(f"  Searching: '{q}'...")
            driver.get(url)
            time.sleep(2.5)

            soup = BeautifulSoup(driver.page_source, "html.parser")
            rows = soup.select("tr.tCenter.hl-tr")
            if not rows:
                continue

            candidates = []
            for r in rows:
                cat_el = r.select_one("a.gen")
                cat_text = cat_el.text.strip() if cat_el else ""
                if not is_pc_category(cat_text):
                    continue

                t_el = r.select_one("a.tt-text, a.tLink")
                if not t_el:
                    continue
                t_title = t_el.text.strip()
                if not is_pc_title(t_title):
                    continue

                href = t_el.get("href", "")
                tid = clean_topic_id(href)
                if not tid:
                    continue

                sz_el = r.select_one("td.tor-size")
                size_str = sz_el.text.strip() if sz_el else ""
                size_bytes = parse_size_bytes(size_str)

                seed_el = r.select_one("b.seedmed, td.seedmed")
                seeds = int(seed_el.text.strip()) if seed_el and seed_el.text.strip().isdigit() else 0

                # Score
                score = seeds * 2
                t_low = t_title.lower()
                if "portable" in t_low or "rip" in t_low:
                    score += 50
                if "repack" in t_low:
                    score += 20
                if "gog" in t_low:
                    score += 30

                candidates.append({
                    "topic_id": tid,
                    "title": t_title,
                    "category": cat_text,
                    "size_bytes": size_bytes,
                    "size_str": size_str,
                    "seeds": seeds,
                    "score": score,
                    "href": href,
                })

            candidates.sort(key=lambda x: (x["score"], x["seeds"]), reverse=True)
            if not candidates:
                continue

            # Check top candidate
            top = candidates[0]
            print(f"    Top candidate: [{top['seeds']} seeds, {top['size_str']}] {top['title'][:65]} (tid={top['topic_id']})")
            # Navigate to topic to get magnet
            driver.get(f"https://rutracker.org/forum/viewtopic.php?t={top['topic_id']}")
            time.sleep(2)
            t_soup = BeautifulSoup(driver.page_source, "html.parser")
            mag_el = t_soup.select_one("a.med.magnet-link, a[href^='magnet:?xt=urn:btih:']")
            if not mag_el:
                print(f"    [!] No magnet on topic page {top['topic_id']}")
                continue

            magnet = mag_el.get("href", "").strip()
            m = re.search(r"urn:btih:([0-9a-fA-F]{40})", magnet)
            if not m:
                continue

            info_hash = m.group(1).upper()
            top["magnet"] = magnet
            top["info_hash"] = info_hash
            chosen = top
            print(f"    [+] Selected! info_hash={info_hash}")
            break

        if chosen:
            selected[game_name] = chosen
        else:
            print(f"  [X] Could not find suitable release for {game_name}")

    driver.quit()

    out_path = REPO_ROOT / "resources" / "autorun" / "scraped_torrents.json"
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(selected, f, indent=2, ensure_ascii=False)

    print("\n" + "=" * 60)
    print(f"[*] Done! Found {len(selected)} / {len(GAMES)} releases.")
    print(f"[*] Saved to {out_path}")
    print("=" * 60)

if __name__ == "__main__":
    main()
