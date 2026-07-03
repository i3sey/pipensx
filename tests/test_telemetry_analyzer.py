#!/usr/bin/env python3

import importlib.util
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "throughput_analyzer", ROOT / "tools" / "analyze_throughput_log.py"
)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader
SPEC.loader.exec_module(MODULE)


def line(stage, **fields):
    values = " ".join(f"{key}={value}" for key, value in fields.items())
    return f"[  12345] [telemetry] schema=1 stage={stage} tag=test {values}"


class TelemetryAnalyzerTests(unittest.TestCase):
    def test_detects_ncm_backpressure(self):
        text = "\n".join([
            line("control", enabled=1, generation=2),
            line("torrent", interval_ms=5000, rx_bps=4000000,
                 verified_bps=4000000, unchoked=4, inflight=120,
                 expired=0, oldest_request_ms=1000,
                 piece_cb_busy_permille=100),
            line("buffer", interval_ms=5000, sink_bps=4000000,
                 processed_bps=800000, pauses=3, pause_max_ms=900,
                 gate_paused=0, high_bytes=950, limit_bytes=1000),
            line("decode", interval_ms=5000, input_bps=800000,
                 output_bps=1200000, zstd_us=200000, aes_us=100000,
                 writer_us=3500000),
            line("ncm", interval_ms=5000, bps=800000,
                 ncm_us=3000000, sha_us=300000),
        ])
        report = MODULE.analyze(MODULE.parse_records(text))
        self.assertEqual(report["bottleneck"], "NCM/SD writer")
        self.assertEqual(report["buffer"]["pauses"], 3)
        self.assertEqual(report["buffer"]["pause_max_ms"], 900)

    def test_detects_backpressure_from_legacy_wait_fields(self):
        text = "\n".join([
            line("control", enabled=1, generation=2),
            line("torrent", interval_ms=5000, rx_bps=4000000,
                 verified_bps=4000000, unchoked=4, inflight=120,
                 expired=0, oldest_request_ms=1000,
                 piece_cb_busy_permille=100),
            line("buffer", interval_ms=5000, sink_bps=4000000,
                 processed_bps=800000, waits=3, wait_max_us=900000,
                 high_bytes=950, limit_bytes=1000),
            line("ncm", interval_ms=5000, bps=800000,
                 ncm_us=3000000, sha_us=300000),
        ])
        report = MODULE.analyze(MODULE.parse_records(text))
        self.assertEqual(report["bottleneck"], "NCM/SD writer")
        self.assertEqual(report["buffer"]["pauses"], 3)
        self.assertEqual(report["buffer"]["pause_max_ms"], 900)

    def test_detects_slow_peer(self):
        text = "\n".join([
            line("control", enabled=1, generation=3),
            line("torrent", interval_ms=5000, rx_bps=700000,
                 verified_bps=500000, unchoked=3, inflight=80,
                 expired=12, oldest_request_ms=15000,
                 piece_cb_busy_permille=20),
            line("buffer", interval_ms=5000, sink_bps=500000,
                 processed_bps=500000, pauses=0, pause_max_ms=0,
                 gate_paused=0, high_bytes=100, limit_bytes=1000),
        ])
        report = MODULE.analyze(MODULE.parse_records(text))
        self.assertEqual(
            report["bottleneck"], "torrent scheduler / slow critical peer"
        )

    def test_uses_last_enabled_session(self):
        text = "\n".join([
            line("control", enabled=1, generation=2),
            line("torrent", interval_ms=5000, rx_bps=1),
            line("control", enabled=0, generation=3),
            line("control", enabled=1, generation=4),
            line("torrent", interval_ms=5000, rx_bps=99),
        ])
        records = MODULE.parse_records(text)
        self.assertEqual(len(records), 2)
        self.assertEqual(records[-1]["rx_bps"], 99)

    def test_uses_decode_summary_and_marks_missing_ncm(self):
        text = "\n".join([
            line("control", enabled=1, generation=5),
            line("torrent", interval_ms=5000, rx_bps=100000,
                 verified_bps=100000, unchoked=2, inflight=32,
                 expired=0, oldest_request_ms=100,
                 piece_cb_busy_permille=5),
            line("decode", event="summary", interval_ms=10000,
                 zstd_us=300000, aes_us=700000, writer_us=2000000),
        ])
        report = MODULE.analyze(MODULE.parse_records(text))
        self.assertEqual(report["busy_permille"]["decode"], 100)
        self.assertEqual(report["busy_permille"]["writer_callback"], 200)
        self.assertIsNone(report["busy_permille"]["ncm_sha"])

    def test_reports_peer_recovery_activity(self):
        text = "\n".join([
            line("control", enabled=1, generation=6),
            line("torrent", interval_ms=5000, rx_bps=300000,
                 verified_bps=200000, unchoked=2, inflight=64,
                 expired=4, oldest_request_ms=12000,
                 hedged=3, cancelled=2, released=5,
                 piece_cb_busy_permille=10),
            line("peer", event="request_timeout", expired=4, strikes=2),
            line("peer", interval_ms=5000, rx_bps=1000,
                 expired=4, strikes=2),
        ])
        report = MODULE.analyze(MODULE.parse_records(text))
        self.assertEqual(report["requests"]["hedged"], 3)
        self.assertEqual(report["requests"]["cancelled"], 2)
        self.assertEqual(report["requests"]["released"], 5)
        self.assertEqual(report["requests"]["peer_timeout_events"], 1)
        self.assertEqual(report["requests"]["max_peer_strikes"], 2)

    def test_summarizes_always_on_diagnostics(self):
        text = "\n".join([
            "[diagnostic] schema=1 level=error stage=image tag=load error=timeout",
            "[diagnostic] schema=1 level=error stage=catalog tag=refresh error=http failure",
            "[diagnostic] schema=1 level=snapshot stage=app tag=manual active=2",
        ])
        diagnostics = MODULE.parse_diagnostics(text)
        summary = MODULE.analyze_diagnostics(diagnostics)
        self.assertEqual(summary["records"], 3)
        self.assertEqual(summary["errors"], 2)
        self.assertEqual(summary["snapshots"], 1)
        self.assertEqual(summary["errors_by_stage"], {"catalog": 1, "image": 1})
        self.assertEqual(summary["latest_error"]["stage"], "catalog")
        self.assertEqual(summary["latest_error"]["error"], "http failure")


if __name__ == "__main__":
    unittest.main()
