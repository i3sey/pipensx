#!/usr/bin/env python3
"""
Interactive RuTracker torrent picker using Chromium + undetected_chromedriver.
Logs in via browser session, then searches for real, working portable PC releases.
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

REPO_ROOT = Path(__file__).resolve().parents[2]
CURATED_PATH = REPO_ROOT / "resources" / "autorun" / "curated_torrents.json"
SWITCH_GAMES_PATH = REPO_ROOT / "resources" / "catalog" / "switch_games.json"
CATALOG_REPO = Path.home() / "pipensx-catalog"
ENV_PATH = CATALOG_REPO / ".env"

AUTORUN_TARGETS = [
    {
        "title": "FlatOut 2",
        "search_queries": ["FlatOut 2 portable", "FlatOut 2 GOG", "FlatOut 2"],
        "executable": "FlatOut2.exe",
        "target_folder": "FlatOut2",
    },
    {
        "title": "Deus Ex: Human Revolution",
        "search_queries": ["Deus Ex Human Revolution Director portable", "Deus Ex Human Revolution portable", "Deus Ex Human Revolution"],
        "executable": "DXHR.exe",
        "target_folder": "DeusExHumanRevolution",
    },
    {
        "title": "Barnyard",
        "search_queries": ["Barnyard Рога и копыта", "Barnyard portable", "Barnyard"],
        "executable": "Barnyard.exe",
        "target_folder": "Barnyard",
    },
    {
        "title": "Dead Space",
        "search_queries": ["Dead Space 2008 portable", "Dead Space 2008 GOG", "Dead Space 2008"],
        "executable": "Dead Space.exe",
        "target_folder": "DeadSpace",
    },
    {
        "title": "Condemned: Criminal Origins",
        "search_queries": ["Condemned Criminal Origins portable", "Condemned Criminal Origins GOG", "Condemned Criminal Origins"],
        "executable": "Condemned.exe",
        "target_folder": "Condemned",
    },
    {
        "title": "Deus Ex: Game of the Year Edition",
        "search_queries": ["Deus Ex Game of the Year GOG", "Deus Ex GOTY", "Deus Ex 2000"],
        "executable": "System/DeusEx.exe",
        "target_folder": "DeusEx",
    },
    {
        "title": "Ultimate Spider-Man",
        "search_queries": ["Ultimate Spider-Man portable", "Ultimate Spider-Man"],
        "executable": "USM.exe",
        "target_folder": "UltimateSpiderMan",
    },
    {
        "title": "Need For Speed: Most Wanted",
        "search_queries": ["Need For Speed Most Wanted 2005 portable", "Need For Speed Most Wanted Black Edition portable", "Need For Speed Most Wanted 2005"],
        "executable": "speed.exe",
        "target_folder": "NFSMostWanted",
    },
    {
        "title": "Need For Speed: Carbon",
        "search_queries": ["Need For Speed Carbon portable", "Need For Speed Carbon Collector portable", "Need For Speed Carbon"],
        "executable": "NFSC.exe",
        "target_folder": "NFSCarbon",
    },
    {
        "title": "Need For Speed: Undercover",
        "search_queries": ["Need For Speed Undercover portable", "Need For Speed Undercover GOG", "Need For Speed Undercover"],
        "executable": "nfs.exe",
        "target_folder": "NFSUndercover",
    },
    {
        "title": "Devil May Cry 4",
        "search_queries": ["Devil May Cry 4 portable", "Devil May Cry 4 DX10", "Devil May Cry 4"],
        "executable": "DevilMayCry4_DX9.exe",
        "target_folder": "DevilMayCry4",
    },
    {
        "title": "Silent Hill 2 Enhanced Edition",
        "search_queries": ["Silent Hill 2 Director's Cut portable", "Silent Hill 2 Enhanced Edition", "Silent Hill 2 PC"],
        "executable": "sh2pc.exe",
        "target_folder": "SilentHill2",
    },
    {
        "title": "Faster Than Light (FTL)",
        "search_queries": ["FTL Faster Than Light GOG", "FTL Faster Than Light portable", "Faster Than Light GOG"],
        "executable": "FTLGame.exe",
        "target_folder": "FTL",
    },
    {
        "title": "Dead Space 2",
        "search_queries": ["Dead Space 2 portable", "Dead Space 2 GOG", "Dead Space 2"],
        "executable": "deadspace2.exe",
        "target_folder": "DeadSpace2",
    },
    {
        "title": "Dead Space 3",
        "search_queries": ["Dead Space 3 portable", "Dead Space 3"],
        "executable": "deadspace3.exe",
        "target_folder": "DeadSpace3",
    },
    {
        "title": "X-Men Origins: Wolverine",
        "search_queries": ["X-Men Origins Wolverine portable", "X-Men Origins Wolverine Uncaged", "X-Men Origins Wolverine"],
        "executable": "Binaries/Wolverine.exe",
        "target_folder": "Wolverine",
    },
    {
        "title": "Spider-Man: Web of Shadows",
        "search_queries": ["Spider-Man Web of Shadows portable", "Spider-Man Web of Shadows"],
        "executable": "Spider-Man Web of Shadows.exe",
        "target_folder": "WebOfShadows",
    },
    {
        "title": "The Path",
        "search_queries": ["The Path Tale of Tales GOG", "The Path Tale of Tales portable", "The Path 2009"],
        "executable": "ThePath.exe",
        "target_folder": "ThePath",
    },
    {
        "title": "Grand Theft Auto IV",
        "search_queries": ["Grand Theft Auto IV Complete Edition portable", "GTA IV Complete Edition portable", "Grand Theft Auto IV portable"],
        "executable": "GTAIV.exe",
        "target_folder": "GTAIV",
    },
    {
        "title": "Deadpool",
        "search_queries": ["Deadpool portable", "Deadpool PC Repack", "Deadpool 2013"],
        "executable": "Binaries/DP.exe",
        "target_folder": "Deadpool",
    },
]

# PC game forums on RuTracker
PC_FORUMS = {
    "46",   # Игры для Windows (старые игры)
    "136",  # Гонки
    "138",  # Аркады
    "140",  # RPG
    "522",  # Экшены / Шутеры
    "1308", # Симуляторы
    "2076", # Квесты и приключения
    "2159", # Горячие Новинки
    "2160", # Шутеры
    "2161", # Экшены
    "2162", # Стратегии
    "2163", # RPG
    "2164", # Инди-игры
}

def parse_size_bytes(text: str) -> int:
    text = text.replace("&nbsp;", " ").replace("\xa0", " ").strip()
    m = re.match(r"^([\d.]+)\s*([KkMmGgTt]?[Bb]|[КкМмГгТт]?[Бб])$", text)
    if not m:
        # maybe pure bytes integer
        digits = re.sub(r"[^\d]", "", text)
        return int(digits) if digits else 0
    val = float(m.group(1))
    unit = m.group(2).upper()
    multiplier = 1
    if "K" in unit or "К" in unit:
        multiplier = 1024
    elif "M" in unit or "М" in unit:
        multiplier = 1024 ** 2
    elif "G" in unit or "Г" in unit:
        multiplier = 1024 ** 3
    elif "T" in unit or "Т" in unit:
        multiplier = 1024 ** 4
    return int(val * multiplier)

def clean_forum_id(href: str) -> str:
    m = re.search(r"viewforum\.php\?f=(\d+)", href)
    return m.group(1) if m else ""

def clean_topic_id(href: str) -> int:
    m = re.search(r"viewtopic\.php\?t=(\d+)", href)
    return int(m.group(1)) if m else 0

def pick_torrents():
    import undetected_chromedriver as uc
    options = uc.ChromeOptions()
    options.add_argument("--disable-gpu")
    options.add_argument("--disable-dev-shm-usage")
    options.add_argument("--no-first-run")

    print("[*] Launching Chromium (version_main=153)...")
    driver = uc.Chrome(
        options=options,
        browser_executable_path="/usr/bin/chromium",
        version_main=153,
        headless=False,
        use_subprocess=True
    )

    driver.get("https://rutracker.org/forum/index.php")
    time.sleep(2)

    def is_logged_in():
        cookies = {c["name"]: c["value"] for c in driver.get_cookies()}
        return "bb_session" in cookies and cookies["bb_session"] != ""

    if not is_logged_in():
        print("[!] Not logged in. Navigating to login page...")
        driver.get("https://rutracker.org/forum/login.php")
        print("[*] Please log in to your RuTracker account in the browser window.")
        print("[*] Waiting for login (timeout 180s)...")
        start = time.time()
        while time.time() - start < 180:
            time.sleep(2)
            if is_logged_in() or "Вы зашли как" in driver.page_source:
                print("[+] Successfully logged in!")
                break
        else:
            print("[X] Login timed out. Exiting.")
            driver.quit()
            return 1

    # Save fresh cookies to .env
    cookies = {c["name"]: c["value"] for c in driver.get_cookies()}
    cookie_str = ";".join(f"{k}={v}" for k, v in cookies.items())
    with open(ENV_PATH, "w") as f:
        f.write(f'RUTRACKER_COOKIE="{cookie_str}"\n')
    print(f"[+] Saved cookies to {ENV_PATH}")

    results = {}
    for target in AUTORUN_TARGETS:
        game_title = target["title"]
        print(f"\n" + "=" * 60)
        print(f"[*] Searching for: {game_title}")
        print("=" * 60)

        found_candidate = None
        for query in target["search_queries"]:
            print(f"  -> Query: '{query}'")
            search_url = "https://rutracker.org/forum/tracker.php?nm=" + urllib.parse.quote(query) + "&o=10&s=2"
            driver.get(search_url)
            time.sleep(2.5)

            soup = BeautifulSoup(driver.page_source, "html.parser")
            rows = soup.select("tr.tCenter.hl-tr")
            if not rows:
                print("     [0 results]")
                continue

            candidates = []
            for r in rows:
                f_link = r.select_one("a.gen")
                forum_href = f_link.get("href", "") if f_link else ""
                fid = clean_forum_id(forum_href)

                t_link = r.select_one("a.tt-text, a.tLink")
                if not t_link:
                    continue
                href = t_link.get("href", "")
                tid = clean_topic_id(href)
                t_title = t_link.text.strip()

                s_el = r.select_one("td.tor-size")
                size_str = s_el.text.strip() if s_el else ""
                size_bytes = parse_size_bytes(size_str)

                seed_el = r.select_one("b.seedmed, td.seedmed")
                seeds = int(seed_el.text.strip()) if seed_el and seed_el.text.strip().isdigit() else 0

                # Score candidate
                score = 0
                t_lower = t_title.lower()

                # Prefer PC forums
                if fid in PC_FORUMS:
                    score += 50
                elif any(cons in t_lower for cons in ["ps2", "ps3", "xbox", "switch", "android", "ios", "macos"]):
                    continue # Skip console/mobile releases

                if "portable" in t_lower:
                    score += 40
                if "gog" in t_lower:
                    score += 30
                if "repack" in t_lower:
                    score += 15
                if seeds > 0:
                    score += min(seeds, 20)

                candidates.append({
                    "topic_id": tid,
                    "title": t_title,
                    "size_bytes": size_bytes,
                    "size_str": size_str,
                    "seeds": seeds,
                    "score": score,
                    "href": href,
                })

            candidates.sort(key=lambda x: x["score"], reverse=True)
            if candidates:
                # Inspect top candidate topic page to extract magnet and check validity
                for cand in candidates[:3]:
                    tid = cand["topic_id"]
                    topic_url = f"https://rutracker.org/forum/viewtopic.php?t={tid}"
                    print(f"     Inspecting topic {tid} (score={cand['score']}, seeds={cand['seeds']}): {cand['title'][:60]}...")
                    driver.get(topic_url)
                    time.sleep(2)

                    t_soup = BeautifulSoup(driver.page_source, "html.parser")
                    magnet_link = t_soup.select_one("a.med.magnet-link, a[href^='magnet:?xt=urn:btih:']")
                    if not magnet_link:
                        print("       [!] No magnet link found on topic page.")
                        continue

                    magnet = magnet_link.get("href", "").strip()
                    m = re.search(r"urn:btih:([0-9a-fA-F]{40})", magnet)
                    if not m:
                        print(f"       [!] Magnet does not contain 40-char SHA1 hex: {magnet[:50]}")
                        continue

                    info_hash = m.group(1).upper()
                    cand["magnet"] = magnet
                    cand["info_hash"] = info_hash
                    found_candidate = cand
                    print(f"       [+] Selected! info_hash={info_hash}, size={cand['size_bytes']} bytes ({cand['size_str']})")
                    break

            if found_candidate:
                break

        if found_candidate:
            results[game_title] = found_candidate
        else:
            print(f"  [X] Failed to find suitable release for {game_title}")

    # Summary
    print("\n" + "=" * 60)
    print(f"[*] Summary: Found {len(results)} / {len(AUTORUN_TARGETS)} torrents.")
    print("=" * 60)
    for g, res in results.items():
        print(f"{g:<32} -> TID: {res['topic_id']:<8} Hash: {res['info_hash']} ({res['seeds']} seeds, {res['size_str']})")

    out_file = REPO_ROOT / "resources" / "autorun" / "scraped_torrents.json"
    with open(out_file, "w", encoding="utf-8") as f:
        json.dump(results, f, ensure_ascii=False, indent=2)
    print(f"\n[+] Saved scraped torrents to {out_file}")

    driver.quit()
    return 0

if __name__ == "__main__":
    sys.exit(pick_torrents())
