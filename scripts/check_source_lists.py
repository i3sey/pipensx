#!/usr/bin/env python3
"""Cross-check CORE_SOURCES between Makefile.pc and CMakeLists.txt.

A drift here means the PC CLI/tests and the Switch/golden builds compile
different cores. APP_SERVICE_SOURCES / UI_SOURCES live only in CMake
(Switch + golden_runner); Makefile.pc pulls those in per-test.
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def parse_make_list(text: str, name: str) -> list[str]:
    m = re.search(
        rf"^{re.escape(name)}\s*=\s*\\?\s*\n((?:[ \t]+.+\n)+)",
        text,
        flags=re.M,
    )
    if not m:
        raise SystemExit(f"Makefile.pc: {name} list not found")
    files: list[str] = []
    for line in m.group(1).splitlines():
        tok = line.strip().rstrip("\\").strip()
        if tok:
            files.append(tok)
    return files


def parse_cmake_set(text: str, name: str) -> list[str]:
    m = re.search(
        rf"set\(\s*{re.escape(name)}\s*((?:.|\n)*?)\)",
        text,
    )
    if not m:
        raise SystemExit(f"CMakeLists.txt: set({name} ...) not found")
    files = re.findall(r"(src/[^\s)]+|vendor/[^\s)]+)", m.group(1))
    return files


def main() -> int:
    make = (ROOT / "Makefile.pc").read_text()
    cmake = (ROOT / "CMakeLists.txt").read_text()

    make_core = parse_make_list(make, "CORE_SOURCES")
    cmake_core = parse_cmake_set(cmake, "CORE_SOURCES")

    errors: list[str] = []

    if len(cmake_core) != len(set(cmake_core)):
        dupes = sorted({f for f in cmake_core if cmake_core.count(f) > 1})
        errors.append(f"CMakeLists.txt CORE_SOURCES has duplicates: {dupes}")

    if len(make_core) != len(set(make_core)):
        dupes = sorted({f for f in make_core if make_core.count(f) > 1})
        errors.append(f"Makefile.pc CORE_SOURCES has duplicates: {dupes}")

    a, b = set(make_core), set(cmake_core)
    only_make = sorted(a - b)
    only_cmake = sorted(b - a)
    if only_make or only_cmake:
        errors.append("CORE_SOURCES mismatch between Makefile.pc and CMakeLists.txt")
        if only_make:
            errors.append("  only in Makefile.pc: " + ", ".join(only_make))
        if only_cmake:
            errors.append("  only in CMakeLists.txt: " + ", ".join(only_cmake))

    # Order need not match, but report sorted for stable diffs.
    if not errors and make_core != cmake_core:
        # Same set, different order — fine.
        pass

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print(f"OK: CORE_SOURCES ({len(make_core)} files) match Makefile.pc ↔ CMakeLists.txt")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
