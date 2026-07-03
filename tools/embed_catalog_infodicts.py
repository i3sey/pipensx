#!/usr/bin/env python3
"""Pre-resolve magnet info dictionaries and embed them into the catalog
(RF_ACCESS_PLAN П2.1).

For every catalog entry the script announces the magnet's info hash to the
RuTracker trackers, fetches the bencoded info dictionary from a peer via
BEP 10 / BEP 9 (ut_metadata), SHA-1-verifies it against the magnet hash and
stores it base64-encoded in the entry's "info_dict" field. Clients with a
populated info_dict skip the tracker -> peer -> ut_metadata phase entirely
and go straight to peer discovery for the download.

Run outside RF or pass --proxy (the tracker announce goes through it; the
peer connections are direct). The client refuses catalog files larger than
16 MiB, so --budget defaults to a safe embedded-bytes total.
"""
import argparse
import base64
import hashlib
import json
import os
import random
import re
import socket
import struct
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor


TRACKERS = [
    "http://bt.t-ru.org/ann?magnet",
    "http://bt2.t-ru.org/ann?magnet",
    "http://bt3.t-ru.org/ann?magnet",
    "http://bt4.t-ru.org/ann?magnet",
]

METADATA_PIECE = 16 * 1024
METADATA_LIMIT = 8 * 1024 * 1024
CLIENT_CATALOG_LIMIT = 16 * 1024 * 1024


def info_hash_from_magnet(magnet):
    match = re.search(r"btih:([0-9A-Fa-f]{40})", magnet or "")
    return match.group(1).upper() if match else ""


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


def bencode(value):
    if isinstance(value, int):
        return b"i%de" % value
    if isinstance(value, bytes):
        return b"%d:%s" % (len(value), value)
    if isinstance(value, str):
        return bencode(value.encode())
    if isinstance(value, dict):
        out = b"d"
        for key in sorted(value):
            out += bencode(key) + bencode(value[key])
        return out + b"e"
    if isinstance(value, list):
        return b"l" + b"".join(bencode(item) for item in value) + b"e"
    raise ValueError("unsupported bencode type")


def quote_hash(hex_hash):
    return urllib.parse.quote_from_bytes(bytes.fromhex(hex_hash), safe="")


def announce(hex_hash, timeout, opener):
    """Collect compact peers from the RuTracker trackers."""
    peer_id = b"-PNEMBED-" + random.randbytes(11)
    peers = []
    seen = set()
    failure = ""
    for url in TRACKERS:
        separator = "&" if "?" in url else "?"
        query = (
            f"{separator}info_hash={quote_hash(hex_hash)}"
            f"&peer_id={urllib.parse.quote_from_bytes(peer_id, safe='')}"
            "&port=6881&uploaded=0&downloaded=0&left=0&compact=1"
            "&event=started&numwant=200"
        )
        request = urllib.request.Request(
            url + query, headers={"User-Agent": "pipensx-embed"})
        try:
            with opener.open(request, timeout=timeout) as response:
                body = response.read(1024 * 1024)
            root, _ = bdecode(body)
        except (OSError, urllib.error.URLError, ValueError) as exc:
            failure = failure or str(exc)
            continue
        reason = root.get(b"failure reason")
        if isinstance(reason, bytes):
            failure = failure or reason.decode("utf-8", "replace")
            if b"not registered" in reason.lower():
                return [], failure
            continue
        compact = root.get(b"peers", b"")
        if isinstance(compact, bytes):
            for i in range(0, len(compact) - 5, 6):
                item = compact[i:i + 6]
                if item not in seen:
                    seen.add(item)
                    peers.append(item)
        if len(peers) >= 100:
            break
    return peers, failure


def read_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("peer closed connection")
        data += chunk
    return data


def read_message(sock):
    length = struct.unpack(">I", read_exact(sock, 4))[0]
    if length > METADATA_PIECE + 1024:
        raise ValueError("oversized peer message")
    return read_exact(sock, length) if length else b""


def send_extended(sock, ext_id, payload, trailer=b""):
    body = bytes([20, ext_id]) + payload + trailer
    sock.sendall(struct.pack(">I", len(body)) + body)


LOCAL_UT_METADATA_ID = 1


def fetch_metadata_from_peer(peer, hex_hash, timeout):
    """BEP 10 handshake + BEP 9 ut_metadata fetch from one compact peer."""
    host = socket.inet_ntoa(peer[:4])
    port = struct.unpack(">H", peer[4:6])[0]
    if not port:
        return None
    info_hash = bytes.fromhex(hex_hash)
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        reserved = bytes([0, 0, 0, 0, 0, 0x10, 0, 0])
        peer_id = b"-PNEMBED-" + random.randbytes(11)
        sock.sendall(b"\x13BitTorrent protocol" + reserved + info_hash +
                     peer_id)
        response = read_exact(sock, 68)
        if response[:20] != b"\x13BitTorrent protocol":
            return None
        if response[28:48] != info_hash or not response[25] & 0x10:
            return None

        send_extended(sock, 0, bencode(
            {b"m": {b"ut_metadata": LOCAL_UT_METADATA_ID}}))

        peer_ut_id = None
        metadata_size = None
        pieces = {}
        deadline = time.monotonic() + timeout * 4
        while time.monotonic() < deadline:
            message = read_message(sock)
            if len(message) < 2 or message[0] != 20:
                continue
            ext_id = message[1]
            payload = message[2:]
            if ext_id == 0:
                handshake, _ = bdecode(payload)
                extensions = handshake.get(b"m", {})
                peer_ut_id = extensions.get(b"ut_metadata")
                metadata_size = handshake.get(b"metadata_size")
                if not peer_ut_id or not isinstance(metadata_size, int):
                    return None
                if metadata_size <= 0 or metadata_size > METADATA_LIMIT:
                    return None
                total = -(-metadata_size // METADATA_PIECE)
                for piece in range(total):
                    send_extended(sock, peer_ut_id, bencode(
                        {b"msg_type": 0, b"piece": piece}))
                continue
            # Peers address extended messages back with the id WE advertised.
            if ext_id != LOCAL_UT_METADATA_ID or metadata_size is None:
                continue
            header, end = bdecode(payload)
            if header.get(b"msg_type") != 1:
                if header.get(b"msg_type") == 2:
                    return None  # peer rejected the request
                continue
            pieces[header[b"piece"]] = payload[end:]
            total = -(-metadata_size // METADATA_PIECE)
            if len(pieces) == total:
                blob = b"".join(pieces[i] for i in range(total))
                if len(blob) != metadata_size:
                    return None
                if hashlib.sha1(blob).hexdigest().upper() != hex_hash:
                    return None
                return blob
        return None


def resolve_entry(item, args, opener):
    hex_hash = info_hash_from_magnet(item.get("magnetURI", ""))
    if not hex_hash:
        return None, "invalid magnet"
    peers, failure = announce(hex_hash, args.timeout, opener)
    if not peers:
        return None, failure or "no peers"
    random.shuffle(peers)
    # The RuTracker swarm is thin and slow, so probe peers concurrently the
    # way the C++ client does — one good peer among many dead ones resolves
    # the metadata in seconds instead of after a long sequential timeout walk.
    found = {}
    stop = threading.Event()
    last = "no metadata from peers"

    def worker(peer):
        if stop.is_set():
            return
        try:
            blob = fetch_metadata_from_peer(peer, hex_hash, args.timeout)
        except (OSError, ValueError, KeyError, ConnectionError):
            return
        if blob:
            found["blob"] = blob
            stop.set()

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        list(pool.map(worker, peers[:args.peers]))
    if "blob" in found:
        return found["blob"], ""
    return None, last


def main():
    parser = argparse.ArgumentParser(
        description="Embed pre-resolved info dictionaries into the catalog")
    parser.add_argument("--catalog", default="resources/catalog/catalog.json")
    parser.add_argument("--output", default="")
    parser.add_argument("--limit", type=int, default=0,
                        help="stop after embedding this many entries")
    parser.add_argument("--timeout", type=int, default=10)
    parser.add_argument("--peers", type=int, default=30,
                        help="max peers to try per entry")
    parser.add_argument("--workers", type=int, default=12,
                        help="concurrent peer connections per entry")
    parser.add_argument("--proxy", default=os.environ.get("HTTPS_PROXY") or
                        os.environ.get("HTTP_PROXY") or "",
                        help="HTTP proxy for the tracker announce")
    parser.add_argument("--only-missing", action="store_true",
                        help="skip entries that already carry an info_dict")
    parser.add_argument("--max-entry-bytes", type=int, default=1024 * 1024,
                        help="skip dictionaries larger than this")
    parser.add_argument("--budget", type=int, default=10 * 1024 * 1024,
                        help="total embedded (decoded) bytes across the "
                             "catalog; the client rejects files over 16 MiB")
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

    embedded_bytes = sum(
        len(item.get("info_dict", "")) * 3 // 4 for item in catalog)
    embedded = 0
    checked = 0
    stats = {}
    for item in catalog:
        if args.limit and embedded >= args.limit:
            break
        if embedded_bytes >= args.budget:
            print(f"budget of {args.budget} embedded bytes reached")
            break
        if args.only_missing and item.get("info_dict"):
            continue
        checked += 1
        blob, reason = resolve_entry(item, args, opener)
        if blob and len(blob) > args.max_entry_bytes:
            blob, reason = None, f"info dict too large ({len(blob)} bytes)"
        if blob:
            item["info_dict"] = base64.b64encode(blob).decode()
            item["metadata_ok"] = True
            embedded += 1
            embedded_bytes += len(blob)
            outcome = f"embedded {len(blob)} bytes"
        else:
            outcome = f"skipped: {reason[:120]}"
        key = "embedded" if blob else "skipped"
        stats[key] = stats.get(key, 0) + 1
        print(f"{checked}: {outcome} {item.get('title', '')[:60]}",
              flush=True)

    output = args.output or args.catalog
    body = json.dumps(catalog, ensure_ascii=False, separators=(",", ":"))
    if len(body.encode()) > CLIENT_CATALOG_LIMIT:
        raise SystemExit(
            f"refusing to write: catalog would be {len(body)} bytes, "
            f"over the client's {CLIENT_CATALOG_LIMIT} limit")
    with open(output, "w", encoding="utf-8") as handle:
        handle.write(body)
    print(f"checked={checked} embedded={embedded} "
          f"embedded_bytes={embedded_bytes} wrote={output} stats={stats}")


if __name__ == "__main__":
    main()
