#!/usr/bin/env python3
"""Cross-implementation check: decode chunks the C++ builder emits.

The C++ test binary (tests/test_bug_report) dumps a real multi-chunk report to
a temp directory; scripts/decode_report.py must reconstruct exactly the tail the
builder encoded. This is what proves the on-device encoder and the host decoder
agree on the wire format. Runs from `make -f Makefile.pc test`.
"""

import importlib.util
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BINARY = ROOT / "tests" / "test_bug_report"

spec = importlib.util.spec_from_file_location(
    "decode_report", ROOT / "scripts" / "decode_report.py"
)
decode_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decode_report)


def main():
    if not BINARY.exists():
        print(f"test_bug_report_decode: {BINARY} not built", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([str(BINARY), tmp], check=True, capture_output=True)
        tmp = pathlib.Path(tmp)
        chunk_files = sorted(tmp.glob("chunk_*.bin"))
        assert chunk_files, "C++ builder dumped no chunks"
        expected = (tmp / "expected.txt").read_bytes().decode("utf-8")

        chunks = [p.read_bytes() for p in chunk_files]
        log, info = decode_report.reassemble(chunks)
        assert log == expected, "decoded log != builder's encoded tail"
        assert info["session"] == 0xBEEF, f"unexpected session {info['session']:04X}"
        assert info["total"] == len(chunk_files)
        assert not info["detailed"]

        # Reverse order + a duplicate must still reconstruct.
        shuffled = list(reversed(chunks)) + [chunks[0]]
        log2, _ = decode_report.reassemble(shuffled)
        assert log2 == expected, "out-of-order/duplicate reassembly failed"

        # A missing chunk must be reported, not silently truncated.
        if len(chunks) > 1:
            try:
                decode_report.reassemble(chunks[:-1])
            except decode_report.ReportError:
                pass
            else:
                raise AssertionError("missing chunk was not detected")

    print("bug report decode cross-check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
