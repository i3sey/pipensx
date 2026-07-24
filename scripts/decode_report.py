#!/usr/bin/env python3
"""Reconstruct a pipensx log from a bug-report screenshot.

A reporter opens Settings -> Report a bug, which renders the recent log tail as
a grid of QR codes on one screen, and sends the developer a photo/screenshot of
it. This script decodes those QR codes, reassembles the chunks and prints the
reconstructed log -- no log file ever leaves the console.

Usage:
    scripts/decode_report.py shot.png [more.png ...]      # decode image(s)
    scripts/decode_report.py --raw chunk0.bin chunk1.bin  # raw chunk payloads

Image decoding needs pyzbar (system 'zbar' library) and Pillow:
    pip install pyzbar pillow      # plus:  apt install libzbar0

The --raw mode has no third-party dependency and is used by the test suite to
validate the wire format against the C++ builder (src/app/bug_report.cpp).

Wire format is documented in src/app/bug_report.hpp and MUST stay in lockstep.
"""

import argparse
import struct
import sys
import zlib

MAGIC = b"PZ"
FORMAT_VERSION = 1
HEADER_SIZE = 16


class ReportError(Exception):
    pass


def parse_chunk(data):
    """Validate one chunk's header, return a dict of its fields + payload."""
    if len(data) < HEADER_SIZE:
        raise ReportError("chunk shorter than header")
    if data[0:2] != MAGIC:
        raise ReportError("bad magic (not a pipensx report chunk)")
    if data[2] != FORMAT_VERSION:
        raise ReportError(f"unsupported format version {data[2]}")
    session, idx, total, flags, crc, clen = struct.unpack(">HBBBII", data[3:16])
    return {
        "session": session,
        "idx": idx,
        "total": total,
        "flags": flags,
        "crc": crc,
        "clen": clen,
        "payload": data[HEADER_SIZE:],
    }


def reassemble(chunk_bytes):
    """Given raw chunk byte-strings (any order, possibly duplicated across
    several photos), reconstruct the log. Returns (log_text, info dict)."""
    parsed = [parse_chunk(b) for b in chunk_bytes]
    if not parsed:
        raise ReportError("no report chunks found")

    # A single photo may catch two different reports; group by session and use
    # the one with the most chunks present.
    sessions = {}
    for c in parsed:
        sessions.setdefault(c["session"], {})[c["idx"]] = c
    session = max(sessions, key=lambda s: len(sessions[s]))
    by_idx = sessions[session]
    total = next(iter(by_idx.values()))["total"]

    missing = [i for i in range(total) if i not in by_idx]
    if missing:
        raise ReportError(
            f"report {session:04X}: missing chunk(s) {missing} of {total} "
            f"-- recapture the whole screen"
        )

    crc = by_idx[0]["crc"]
    clen = by_idx[0]["clen"]
    compressed = b"".join(by_idx[i]["payload"] for i in range(total))
    if len(compressed) != clen:
        raise ReportError(
            f"report {session:04X}: reassembled {len(compressed)} bytes, "
            f"header says {clen}"
        )

    log = zlib.decompress(compressed)
    if (zlib.crc32(log) & 0xFFFFFFFF) != crc:
        raise ReportError(f"report {session:04X}: CRC mismatch after inflate")

    info = {
        "session": session,
        "total": total,
        "detailed": bool(by_idx[0]["flags"] & 0x01),
        # The console dropped its repetitive per-image telemetry lines to make
        # the rest of the log fit; the gaps in the timeline are expected.
        "filtered": bool(by_idx[0]["flags"] & 0x02),
        "bytes": len(log),
    }
    return log.decode("utf-8", errors="replace"), info


def chunks_from_images(paths):
    try:
        from PIL import Image
        from pyzbar.pyzbar import ZBarSymbol, decode
    except ImportError as exc:  # pragma: no cover - env dependent
        raise ReportError(
            "image decoding needs pyzbar + Pillow "
            "(pip install pyzbar pillow, apt install libzbar0)"
        ) from exc

    out = []
    for path in paths:
        found = decode(Image.open(path), symbols=[ZBarSymbol.QRCODE])
        if not found:
            print(f"decode_report: no QR codes in {path}", file=sys.stderr)
        out.extend(sym.data for sym in found)
    return out


def chunks_from_raw(paths):
    out = []
    for path in paths:
        with open(path, "rb") as handle:
            out.append(handle.read())
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", help="image(s), or raw chunks with --raw")
    parser.add_argument("--raw", action="store_true",
                        help="treat inputs as raw chunk payloads, not images")
    parser.add_argument("-o", "--out", help="also write the log to this file")
    args = parser.parse_args()

    try:
        chunks = (chunks_from_raw if args.raw else chunks_from_images)(args.files)
        log, info = reassemble(chunks)
    except ReportError as exc:
        print(f"decode_report: {exc}", file=sys.stderr)
        return 1
    except zlib.error as exc:
        print(f"decode_report: inflate failed: {exc}", file=sys.stderr)
        return 1

    mode = "detailed" if info["detailed"] else "default"
    filtered = ", image telemetry dropped" if info["filtered"] else ""
    print(
        f"decode_report: report {info['session']:04X} "
        f"({info['total']} codes, {mode} mode, {info['bytes']} bytes"
        f"{filtered}) OK",
        file=sys.stderr,
    )
    out_path = args.out or f"report-{info['session']:04X}.log"
    with open(out_path, "w", encoding="utf-8") as handle:
        handle.write(log)
    print(f"decode_report: wrote {out_path}", file=sys.stderr)
    sys.stdout.write(log)
    return 0


if __name__ == "__main__":
    sys.exit(main())
