#!/usr/bin/env python3
"""Publish validated, content-addressed Langegen snapshots to GitLab."""

import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


SOURCE_API = "https://api.github.com/repos/Langegen/switch-game-collection"
SOURCE_RAW = "https://raw.githubusercontent.com/Langegen/switch-game-collection/"
REGISTRY = "https://gitlab.com/api/v4/projects/88330126/packages/generic/langegen"
FILES = tuple(f"{lang}_catalog.json" for lang in
              ("RU", "EN", "ES", "FR", "DE", "IT", "PT_BR", "ZH_Hans")) + (
                  "catalog_diff.json",)
MAX_BYTES = 48 * 1024 * 1024


def download(url, limit=MAX_BYTES, missing_ok=False):
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, headers={
                "User-Agent": "pipensx-langegen-mirror",
                "Accept": "application/vnd.github.raw+json",
            })
            with urllib.request.urlopen(request, timeout=60) as response:
                if not response.url.startswith("https://"):
                    raise ValueError("HTTPS redirect required")
                data = response.read(limit + 1)
            if not data or len(data) > limit:
                raise ValueError("Empty or oversized response")
            return data
        except urllib.error.HTTPError as error:
            if error.code == 404 and missing_ok:
                return None
            if error.code < 500:
                raise
            if attempt == 2:
                raise
        except (OSError, TimeoutError):
            if attempt == 2:
                raise
        time.sleep(2 ** attempt)


def validate(file, data):
    if not data or len(data) > MAX_BYTES:
        raise ValueError(f"{file}: empty or oversized payload")
    value = json.loads(data)
    if file == "catalog_diff.json":
        if (not isinstance(value, dict) or value.get("version") != 1 or
                not isinstance(value.get("added_or_updated"), dict) or
                not isinstance(value.get("deleted_topic_ids"), list)):
            raise ValueError("Invalid catalog diff")
    elif (not isinstance(value, list) or not 0 < len(value) <= 20000 or
          any(not isinstance(entry, dict) or not entry.get("title") or
              not isinstance(entry.get("magnet"), str) or
              not entry["magnet"].startswith("magnet:?") for entry in value)):
        raise ValueError(f"{file}: invalid catalog entries")


def prepare(output):
    commit = json.loads(download(f"{SOURCE_API}/commits/main", 1024 * 1024))["sha"]
    if not isinstance(commit, str) or not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Invalid upstream commit")

    def fetch(file):
        data = download(f"{SOURCE_RAW}{commit}/{file}")
        validate(file, data)
        digest = hashlib.sha256(data).hexdigest()
        path = output / f"{file}.zst"
        subprocess.run(["zstd", "-q", "-3", f"--stream-size={len(data)}",
                        "--force", "-o", str(path)],
                       input=data, check=True)
        print(f"{file}: {len(data)} bytes -> {path.stat().st_size}, {digest}",
              flush=True)
        return file, {"sha256": digest, "bytes": len(data)}

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        files = dict(pool.map(fetch, FILES))
    manifest = {"schemaVersion": 1, "sourceCommit": commit, "files": files}
    (output / "manifest.json").write_text(json.dumps(manifest, sort_keys=True) + "\n")
    return manifest


def upload(path, version):
    token = os.environ.get("CI_JOB_TOKEN")
    if token:
        request = urllib.request.Request(
            f"{REGISTRY}/{version}/{path.name}", data=path.read_bytes(),
            method="PUT", headers={"JOB-TOKEN": token,
                                   "Content-Type": "application/octet-stream"})
        with urllib.request.urlopen(request, timeout=120) as response:
            if response.status != 201:
                raise RuntimeError(f"Upload failed: HTTP {response.status}")
    else:
        subprocess.run([
            "glab", "packages", "upload", str(path), "--name", "langegen",
            "--version", version, "--repo", "pipensx-group/pipensx",
        ], check=True)


def publish(output, manifest):
    current = download(f"{REGISTRY}/latest/manifest.json", 64 * 1024, missing_ok=True)
    if current is not None and json.loads(current) == manifest:
        print("Mirror already matches upstream", flush=True)
        return
    for file, entry in manifest["files"].items():
        path = output / f"{file}.zst"
        existing = download(f"{REGISTRY}/{entry['sha256']}/{path.name}", missing_ok=True)
        if existing is not None:
            restored = subprocess.run(
                ["zstd", "-q", "-d", "-c"], input=existing,
                capture_output=True, check=True).stdout
            if (len(restored) != entry["bytes"] or
                    hashlib.sha256(restored).hexdigest() != entry["sha256"]):
                raise ValueError(f"Immutable snapshot differs: {file}")
            continue
        upload(path, entry["sha256"])
    # Readers keep the previous snapshot until every referenced file is available.
    upload(output / "manifest.json", "latest")
    print("Mirror published", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--publish", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="langegen-") as temporary:
        output = args.output or Path(temporary)
        output.mkdir(parents=True, exist_ok=True)
        manifest = prepare(output)
        if args.publish:
            publish(output, manifest)


if __name__ == "__main__":
    main()
