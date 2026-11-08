#!/usr/bin/env python3
"""Resolve the same version for CMake, Make and local Switch builds."""
import argparse
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parent.parent
PATTERN = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?(?:\+[0-9A-Za-z.-]+)?")


def resolve_version(override=None):
    value = override if override is not None else os.environ.get("PIPENSX_VERSION")
    if not value:
        try:
            description = subprocess.check_output(
                ["git", "-c", f"safe.directory={ROOT}", "describe", "--tags", "--long", "--dirty", "--match", "[0-9]*", "--match", "v[0-9]*"],
                cwd=ROOT, stderr=subprocess.DEVNULL, text=True).strip()
            match = re.fullmatch(r"v?([0-9]+\.[0-9]+\.[0-9]+)-(\d+)-g([0-9a-f]+)(-dirty)?", description)
            if match:
                tag, distance, commit, dirty = match.groups()
                value = tag if distance == "0" and not dirty else (
                    f"{tag}-dev.{distance}+g{commit}" + (".dirty" if dirty else ""))
        except (OSError, subprocess.CalledProcessError):
            pass
    if not value:
        value = (ROOT / "VERSION").read_text().strip()
    if value.startswith("v"):
        value = value[1:]
    if len(value) > 64 or not PATTERN.fullmatch(value):
        raise ValueError("version must be major.minor.patch with an optional prerelease/build suffix (max 64 characters)")
    return value


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version")
    parser.add_argument("--numeric", action="store_true")
    args = parser.parse_args()
    try:
        version = resolve_version(args.version)
        print(".".join(PATTERN.fullmatch(version).groups()[:3]) if args.numeric else version)
    except ValueError as error:
        parser.error(str(error))
