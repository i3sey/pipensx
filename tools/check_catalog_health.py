#!/usr/bin/env python3
import argparse
import json
import os
import random
import re
import time
import urllib.error
import urllib.parse
import urllib.request


TRACKERS = [
    "http://bt.t-ru.org/ann?magnet",
    "http://bt2.t-ru.org/ann?magnet",
    "http://bt3.t-ru.org/ann?magnet",
    "http://bt4.t-ru.org/ann?magnet",
]


def info_hash_from_magnet(magnet):
    match = re.search(r"btih:([0-9A-Fa-f]{40})", magnet or "")
    return match.group(1).upper() if match else ""


def tracker_from_magnet(magnet):
    for part in (magnet or "").split("&"):
        if part.startswith("tr="):
            tracker = urllib.parse.unquote_plus(part[3:])
            if tracker in TRACKERS:
                return tracker
    return TRACKERS[0]


def bdecode(data, offset=0):
    token = data[offset:offset + 1]
    if token == b"i":
        end = data.index(b"e", offset)
        return int(data[offset + 1:end]), end + 1
    if token == b"l":
        offset += 1
        out = []
        while data[offset:offset + 1] != b"e":
            value, offset = bdecode(data, offset)
            out.append(value)
        return out, offset + 1
    if token == b"d":
        offset += 1
        out = {}
        while data[offset:offset + 1] != b"e":
            key, offset = bdecode(data, offset)
            value, offset = bdecode(data, offset)
            out[key] = value
        return out, offset + 1
    if token.isdigit():
        colon = data.index(b":", offset)
        size = int(data[offset:colon])
        begin = colon + 1
        return data[begin:begin + size], begin + size
    raise ValueError("invalid bencode")


def quote_hash(hex_hash):
    raw = bytes.fromhex(hex_hash)
    return urllib.parse.quote_from_bytes(raw, safe="")


def announce_once(url, hex_hash, timeout, opener):
    peer_id = b"-PNHEALTH-" + random.randbytes(10)
    separator = "&" if "?" in url else "?"
    query = (
        f"{separator}info_hash={quote_hash(hex_hash)}"
        f"&peer_id={urllib.parse.quote_from_bytes(peer_id, safe='')}"
        "&port=6881&uploaded=0&downloaded=0&left=0&compact=1&event=started"
    )
    request = urllib.request.Request(
        url + query, headers={"User-Agent": "pipensx-health-check"}
    )
    with opener.open(request, timeout=timeout) as response:
        body = response.read(1024 * 1024)
    root, _ = bdecode(body)
    failure = root.get(b"failure reason")
    if isinstance(failure, bytes):
        return 0, failure.decode("utf-8", "replace")
    peers = root.get(b"peers", b"")
    if isinstance(peers, bytes):
        return len(peers) // 6, ""
    if isinstance(peers, list):
        return len(peers), ""
    return 0, ""


def announce(item, timeout, opener):
    hex_hash = info_hash_from_magnet(item.get("magnetURI", ""))
    if not hex_hash:
        return "dead", 0, "Invalid magnet hash"
    first = tracker_from_magnet(item.get("magnetURI", ""))
    trackers = [first] + [url for url in TRACKERS if url != first]
    last_error = ""
    for tracker in trackers:
        try:
            peers, failure = announce_once(tracker, hex_hash, timeout, opener)
        except (OSError, urllib.error.URLError, ValueError) as exc:
            last_error = str(exc)
            continue
        if failure:
            last_error = failure
            if "not registered" in failure.lower():
                return "tracker_not_registered", 0, failure
            continue
        if peers:
            return "ok", peers, ""
    if last_error:
        return "no_peers", 0, last_error[:512]
    return "no_peers", 0, "No peers returned by RuTracker trackers"


def main():
    parser = argparse.ArgumentParser(
        description="Check RuTracker catalog magnets through HTTP announce"
    )
    parser.add_argument("--catalog", default="resources/catalog/catalog.json")
    parser.add_argument("--output", default="")
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--timeout", type=int, default=12)
    parser.add_argument("--only-unknown", action="store_true")
    parser.add_argument("--proxy", default=os.environ.get("HTTPS_PROXY") or
                        os.environ.get("HTTP_PROXY") or "")
    args = parser.parse_args()

    with open(args.catalog, "r", encoding="utf-8") as handle:
        catalog = json.load(handle)

    handlers = []
    if args.proxy:
        handlers.append(urllib.request.ProxyHandler({
            "http": args.proxy,
            "https": args.proxy,
        }))
    opener = urllib.request.build_opener(*handlers)

    checked = 0
    stats = {}
    now = int(time.time())
    for item in catalog:
        if args.only_unknown and item.get("health") not in ("", "unknown", None):
            continue
        if args.limit and checked >= args.limit:
            break
        health, peers, reason = announce(item, args.timeout, opener)
        item["health"] = health
        item["last_checked_at"] = now
        item["peer_count"] = peers
        item["metadata_ok"] = False
        if reason:
            item["failure_reason"] = reason
        else:
            item.pop("failure_reason", None)
        checked += 1
        stats[health] = stats.get(health, 0) + 1
        print(f"{checked}: {health} peers={peers} {item.get('title', '')}",
              flush=True)

    output = args.output or args.catalog
    with open(output, "w", encoding="utf-8") as handle:
        json.dump(catalog, handle, ensure_ascii=False, separators=(",", ":"))
    print(f"checked={checked} wrote={output} stats={stats}")


if __name__ == "__main__":
    main()
