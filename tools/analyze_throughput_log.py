#!/usr/bin/env python3
"""Summarize pipensx schema=1 throughput telemetry."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import statistics
from collections import defaultdict
from pathlib import Path


TELEMETRY_RE = re.compile(r"\[telemetry\]\s+(.*)$")


def parse_value(value: str):
    try:
        return int(value)
    except ValueError:
        return value


def parse_records(text: str) -> list[dict]:
    records: list[dict] = []
    for line in text.splitlines():
        match = TELEMETRY_RE.search(line)
        if not match:
            continue
        record = {}
        try:
            fields = shlex.split(match.group(1))
        except ValueError:
            continue
        for field in fields:
            if "=" not in field:
                continue
            key, value = field.split("=", 1)
            record[key] = parse_value(value)
        if record.get("schema") == 1 and "stage" in record:
            records.append(record)
    enabled = [
        index for index, record in enumerate(records)
        if record.get("stage") == "control" and record.get("enabled") == 1
    ]
    if enabled:
        records = records[enabled[-1]:]
    return records


def percentile(values: list[int], fraction: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    index = round((len(ordered) - 1) * fraction)
    return ordered[index]


def numeric(records: list[dict], key: str) -> list[int]:
    return [
        value for record in records
        if isinstance((value := record.get(key)), int)
    ]


def stage_rate(records: list[dict], key: str) -> dict:
    values = numeric(records, key)
    return {
        "samples": len(values),
        "p10": percentile(values, 0.10),
        "median": int(statistics.median(values)) if values else 0,
        "peak": max(values, default=0),
    }


def busy_permille(records: list[dict], *keys: str) -> int | None:
    values = []
    for record in records:
        interval_ms = record.get("interval_ms")
        if not isinstance(interval_ms, int) or interval_ms <= 0:
            continue
        elapsed_us = sum(
            record.get(key, 0) for key in keys
            if isinstance(record.get(key, 0), int)
        )
        values.append(elapsed_us * 1000 // (interval_ms * 1000))
    return int(statistics.median(values)) if values else None


def analyze(records: list[dict]) -> dict:
    by_stage: dict[str, list[dict]] = defaultdict(list)
    summaries: dict[str, list[dict]] = defaultdict(list)
    for record in records:
        if record.get("event") == "summary":
            summaries[str(record["stage"])].append(record)
        else:
            by_stage[str(record["stage"])].append(record)

    torrent = by_stage.get("torrent", [])
    buffer = by_stage.get("buffer", [])
    decode = by_stage.get("decode", [])
    ncm = by_stage.get("ncm", [])
    peer = [record for record in by_stage.get("peer", [])
            if "rx_bps" in record]
    peer_timeouts = [record for record in by_stage.get("peer", [])
                     if record.get("event") == "request_timeout"]

    rates = {
        "network_rx": stage_rate(torrent, "rx_bps"),
        "verified": stage_rate(torrent, "verified_bps"),
        "buffer_in": stage_rate(buffer, "sink_bps"),
        "buffer_out": stage_rate(buffer, "processed_bps"),
        "decode_in": stage_rate(decode, "input_bps"),
        "decode_out": stage_rate(decode, "output_bps"),
        "ncm": stage_rate(ncm, "bps"),
    }

    waits = sum(numeric(buffer, "waits"))
    wait_max = max(numeric(buffer, "wait_max_us"), default=0)
    high_ratios = []
    for record in buffer:
        high = record.get("high_bytes")
        limit = record.get("limit_bytes")
        if isinstance(high, int) and isinstance(limit, int) and limit > 0:
            high_ratios.append(high * 1000 // limit)
    high_ratio = max(high_ratios, default=0)

    expired = sum(numeric(torrent, "expired"))
    oldest = max(numeric(torrent, "oldest_request_ms"), default=0)
    unchoked = numeric(torrent, "unchoked")
    inflight = numeric(torrent, "inflight")
    decode_busy = busy_permille(decode, "zstd_us", "aes_us")
    if decode_busy is None:
        decode_busy = busy_permille(summaries.get("decode", []),
                                    "zstd_us", "aes_us")
    writer_busy = busy_permille(decode, "writer_us")
    if writer_busy is None:
        writer_busy = busy_permille(summaries.get("decode", []),
                                    "writer_us")
    ncm_busy = busy_permille(ncm, "ncm_us", "sha_us")
    if ncm_busy is None:
        ncm_busy = busy_permille(summaries.get("ncm", []),
                                 "ncm_us", "sha_us")
    piece_values = numeric(torrent, "piece_cb_busy_permille")
    piece_busy = int(statistics.median(piece_values)) if piece_values else None
    hedged = sum(numeric(torrent, "hedged"))
    cancelled = sum(numeric(torrent, "cancelled"))
    released = sum(numeric(torrent, "released"))
    max_strikes = max(numeric(peer, "strikes"), default=0)

    evidence = []
    bottleneck = "insufficient telemetry"
    if rates["network_rx"]["samples"]:
        bottleneck = "no dominant bottleneck detected"
        if waits > 0 or high_ratio >= 900:
            evidence.append(
                f"install buffer pressure: waits={waits}, "
                f"max_wait_ms={wait_max / 1000:.1f}, high={high_ratio / 10:.1f}%"
            )
            if (ncm_busy is not None and ncm_busy >= 400 and
                    (decode_busy is None or ncm_busy >= decode_busy)):
                bottleneck = "NCM/SD writer"
            elif (decode_busy is not None and decode_busy >= 400 and
                  (ncm_busy is None or decode_busy > ncm_busy)):
                bottleneck = "NSZ decompression/AES"
            else:
                bottleneck = "install pipeline backpressure"
        elif expired > 0 or oldest >= 10000:
            bottleneck = "torrent scheduler / slow critical peer"
            evidence.append(
                f"request stalls: expired={expired}, oldest_ms={oldest}"
            )
            if peer_timeouts or max_strikes:
                evidence.append(
                    f"peer penalties: timeout_events={len(peer_timeouts)}, "
                    f"max_strikes={max_strikes}"
                )
        elif unchoked and statistics.median(unchoked) <= 1:
            bottleneck = "swarm connectivity / choked peers"
            evidence.append(
                f"median_unchoked={statistics.median(unchoked):.1f}"
            )
        elif inflight and statistics.median(inflight) < 16:
            bottleneck = "torrent scheduler starvation"
            evidence.append(
                f"median_inflight={statistics.median(inflight):.1f}"
            )
        elif piece_busy is not None and piece_busy >= 500:
            bottleneck = "piece verification / sink callback"
            evidence.append(f"piece_callback_busy={piece_busy / 10:.1f}%")
        elif writer_busy is not None and writer_busy >= 500:
            bottleneck = "installer writer callback"
            evidence.append(f"writer_callback_busy={writer_busy / 10:.1f}%")

    if hedged or cancelled or released:
        evidence.append(
            f"critical request recovery: hedged={hedged}, "
            f"cancelled={cancelled}, released={released}"
        )

    return {
        "records": len(records),
        "rates": rates,
        "busy_permille": {
            "decode": decode_busy,
            "writer_callback": writer_busy,
            "ncm_sha": ncm_busy,
            "piece_callback": piece_busy,
        },
        "buffer": {
            "waits": waits,
            "wait_max_us": wait_max,
            "high_ratio_permille": high_ratio,
        },
        "requests": {
            "expired": expired,
            "oldest_ms": oldest,
            "hedged": hedged,
            "cancelled": cancelled,
            "released": released,
            "peer_timeout_events": len(peer_timeouts),
            "max_peer_strikes": max_strikes,
        },
        "bottleneck": bottleneck,
        "evidence": evidence,
    }


def format_speed(value: int) -> str:
    units = ["B/s", "KiB/s", "MiB/s", "GiB/s"]
    amount = float(value)
    unit = units[0]
    for unit in units:
        if amount < 1024 or unit == units[-1]:
            break
        amount /= 1024
    return f"{amount:.2f} {unit}"


def print_report(report: dict) -> None:
    print("Stage                 samples        p10     median       peak")
    for name, rate in report["rates"].items():
        print(
            f"{name:20} {rate['samples']:7d} "
            f"{format_speed(rate['p10']):>10} "
            f"{format_speed(rate['median']):>10} "
            f"{format_speed(rate['peak']):>10}"
        )
    print()
    print(f"Likely bottleneck: {report['bottleneck']}")
    for evidence in report["evidence"]:
        print(f"- {evidence}")
    busy = report["busy_permille"]
    def busy_text(value: int | None) -> str:
        return "n/a" if value is None else f"{value / 10:.1f}%"
    print(
        "Busy time: "
        f"decode={busy_text(busy['decode'])} "
        f"writer={busy_text(busy['writer_callback'])} "
        f"ncm+sha={busy_text(busy['ncm_sha'])} "
        f"piece={busy_text(busy['piece_callback'])}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Analyze pipensx stream-install throughput telemetry"
    )
    parser.add_argument("log", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    records = parse_records(args.log.read_text(encoding="utf-8", errors="replace"))
    if not records:
        raise SystemExit("No schema=1 telemetry records found")
    report = analyze(records)
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_report(report)


if __name__ == "__main__":
    main()
