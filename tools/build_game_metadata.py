#!/usr/bin/env python3
import argparse
import json
import re
import sys
import urllib.request

TITLEDB_URL = "https://raw.githubusercontent.com/blawar/titledb/master/US.en.json"


def norm(value: str) -> str:
    value = value.lower()
    for old, new in [
        ("™", ""),
        ("®", ""),
        ("©", ""),
        ("’", ""),
        ("'", ""),
        (":", " "),
        ("-", " "),
        ("_", " "),
    ]:
        value = value.replace(old, new)
    value = re.sub(r"\([^)]*\)|\[[^]]*\]", " ", value)
    value = re.sub(r"\+.*$", " ", value)
    value = re.sub(
        r"\b(complete|edition|deluxe|ultimate|bundle|pack|dlc|nsz|nsp|xci|switch|version)\b",
        " ",
        value,
    )
    value = re.sub(r"[^a-z0-9]+", " ", value)
    return " ".join(value.split())


def first_release_title(title: str) -> str:
    return title.split("/")[0].strip()


def load_json(path_or_url: str):
    if path_or_url.startswith("http://") or path_or_url.startswith("https://"):
        req = urllib.request.Request(path_or_url, headers={"User-Agent": "pipensx-metadata-builder"})
        with urllib.request.urlopen(req, timeout=90) as response:
            return json.load(response)
    with open(path_or_url, "r", encoding="utf-8") as handle:
        return json.load(handle)


def short_text(value, limit):
    if not isinstance(value, str):
        return ""
    value = value.strip()
    return value[:limit]


def build(catalog, titledb):
    by_name = {}
    ambiguous = set()
    for item in titledb.values():
        name = item.get("name") or ""
        title_id = item.get("id") or ""
        if not re.fullmatch(r"0100[0-9A-Fa-f]{12}", title_id):
            continue
        key = norm(name)
        if not key or len(key) < 3:
            continue
        if key in by_name:
            ambiguous.add(key)
        else:
            by_name[key] = item
    for key in ambiguous:
        by_name.pop(key, None)

    keys = list(by_name.keys())
    out = []
    seen = set()
    stats = {"exact": 0, "prefix": 0, "unmatched": 0, "ambiguous": 0}
    for entry in catalog:
        title = first_release_title(entry.get("title", ""))
        key = norm(title)
        if not key:
            stats["unmatched"] += 1
            continue
        match = by_name.get(key)
        confidence = "exact" if match else ""
        if not match and len(key) >= 8:
            candidates = [
                by_name[candidate]
                for candidate in keys
                if len(candidate) >= 8
                and (key.startswith(candidate) or candidate.startswith(key))
            ]
            ids = {candidate.get("id") for candidate in candidates}
            if len(ids) == 1:
                match = candidates[0]
                confidence = "prefix"
            elif candidates:
                stats["ambiguous"] += 1
        if not match:
            stats["unmatched"] += 1
            continue
        info_hash = entry.get("magnetURI", "").split("btih:")
        if len(info_hash) < 2:
            stats["unmatched"] += 1
            continue
        info_hash = info_hash[1].split("&")[0].upper()
        if not re.fullmatch(r"[0-9A-F]{40}", info_hash) or info_hash in seen:
            continue
        seen.add(info_hash)
        stats[confidence] += 1
        out.append(
            {
                "infoHash": info_hash,
                "titleId": match.get("id", ""),
                "match": confidence,
                "name": short_text(match.get("name"), 256),
                "intro": short_text(match.get("intro"), 512),
                "description": short_text(match.get("description"), 4096),
                "publisher": short_text(match.get("publisher"), 128),
                "releaseDate": str(match.get("releaseDate") or ""),
                "iconUrl": short_text(match.get("iconUrl"), 1024),
                "bannerUrl": short_text(match.get("bannerUrl"), 1024),
                "screenshots": [
                    url
                    for url in (match.get("screenshots") or [])[:4]
                    if isinstance(url, str) and len(url) <= 1024
                ],
                "categories": [
                    category
                    for category in (match.get("category") or [])[:6]
                    if isinstance(category, str) and len(category) <= 64
                ],
            }
        )
    out.sort(key=lambda item: item["infoHash"])
    return out, stats


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", default="resources/catalog/catalog.json")
    parser.add_argument("--titledb", default=TITLEDB_URL)
    parser.add_argument("--output", default="resources/catalog/game_metadata_index.json")
    args = parser.parse_args()

    catalog = load_json(args.catalog)
    titledb = load_json(args.titledb)
    index, stats = build(catalog, titledb)
    with open(args.output, "w", encoding="utf-8") as handle:
        json.dump(index, handle, ensure_ascii=False, separators=(",", ":"))
    print(
        f"wrote {len(index)} metadata matches to {args.output}; "
        f"exact={stats['exact']} prefix={stats['prefix']} "
        f"ambiguous={stats['ambiguous']} unmatched={stats['unmatched']}",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
