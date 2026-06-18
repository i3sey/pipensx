#!/usr/bin/env python3
import argparse
import datetime
import json
import lzma
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_DUMPER = ROOT / ".tmp" / "switch-dumps-git"
DEFAULT_CATALOG = ROOT / "resources" / "catalog" / "catalog.json"
DEFAULT_METADATA = ROOT / "resources" / "catalog" / "game_metadata_index.json"


def run(cmd, cwd=ROOT):
    print("+", " ".join(str(part) for part in cmd), file=sys.stderr)
    subprocess.run(cmd, cwd=cwd, check=True)


def validate_catalog(path):
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, list) or not data:
        raise SystemExit(f"{path} is not a non-empty catalog array")
    required = {"title", "magnetURI", "size", "published_date", "screenshots"}
    usable = 0
    seen = set()
    for item in data:
        if not isinstance(item, dict) or not required.issubset(item):
            continue
        magnet = item.get("magnetURI", "")
        if "magnet:?xt=urn:btih:" not in magnet:
            continue
        info_hash = magnet.split("btih:", 1)[1].split("&", 1)[0].upper()
        if len(info_hash) != 40 or info_hash in seen:
            continue
        seen.add(info_hash)
        usable += 1
    if usable == 0:
        raise SystemExit(f"{path} contains no usable magnet entries")
    print(f"validated catalog: {len(data)} entries, {usable} usable hashes",
          file=sys.stderr)


def parse_timestamp(value):
    try:
        return int(datetime.datetime.strptime(
            value, "%Y.%m.%d %H:%M:%S").timestamp())
    except Exception:
        return 0


def iter_xml_torrents(xml_path):
    context = ET.iterparse(xml_path, events=("start", "end"))
    context = iter(context)
    _, root = next(context)
    for event, elem in context:
        if event == "end" and elem.tag == "torrent":
            yield elem
            root.clear()


def xml_index_by_hash(xml_path, forum_id):
    indexed = {}
    for elem in iter_xml_torrents(xml_path):
        forum_elem = elem.find("forum")
        torrent_elem = elem.find("torrent")
        if forum_elem is None or torrent_elem is None:
            continue
        if int(forum_elem.attrib.get("id", "0") or 0) != forum_id:
            continue
        info_hash = torrent_elem.attrib.get("hash", "").upper()
        if len(info_hash) != 40:
            continue
        indexed[info_hash] = {
            "topic_id": int(elem.attrib.get("id", "0") or 0),
            "forum_id": forum_id,
            "tracker_id": int(torrent_elem.attrib.get("tracker_id", "0") or 0),
            "source_updated_at": parse_timestamp(
                elem.attrib.get("registred_at", "")),
            "deleted": elem.find("del") is not None,
        }
    return indexed


def enrich_catalog(path, xml_path, forum_id):
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    by_hash = xml_index_by_hash(xml_path, forum_id)
    generated_at = int(time.time())
    for item in data:
        magnet = item.get("magnetURI", "")
        if "btih:" not in magnet:
            continue
        info_hash = magnet.split("btih:", 1)[1].split("&", 1)[0].upper()
        meta = by_hash.get(info_hash, {})
        if meta:
            item["topic_id"] = meta["topic_id"]
            item["forum_id"] = meta["forum_id"]
            item["tracker_id"] = meta["tracker_id"]
            item["source_updated_at"] = meta["source_updated_at"]
            if meta["deleted"]:
                item["health"] = "dead"
                item["failure_reason"] = "Topic is marked deleted in XML dump"
            else:
                item.setdefault("health", "unknown")
        else:
            item.setdefault("health", "unknown")
        item["catalog_generated_at"] = generated_at
        item.setdefault("last_checked_at", 0)
        item.setdefault("peer_count", 0)
        item.setdefault("metadata_ok", False)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(data, handle, ensure_ascii=False, separators=(",", ":"))


def materialize_xml(input_path, tmpdir):
    if input_path.suffix != ".xz":
        return input_path
    output = pathlib.Path(tmpdir) / input_path.name[:-3]
    print(f"+ decompress {input_path} -> {output}", file=sys.stderr)
    with lzma.open(input_path, "rb") as source, output.open("wb") as dest:
        shutil.copyfileobj(source, dest, length=8 * 1024 * 1024)
    return output


def main():
    parser = argparse.ArgumentParser(
        description="Rebuild bundled pipensx catalog from bqio/switch-dumps sources"
    )
    parser.add_argument("xml", type=pathlib.Path,
                        help="RuTracker XML DB dump, e.g. backup.20260425.xml")
    parser.add_argument("--forum-id", type=int, default=1605,
                        help="RuTracker Nintendo Switch forum id")
    parser.add_argument("--dumper", type=pathlib.Path, default=DEFAULT_DUMPER,
                        help="Path to bqio/switch-dumps source checkout")
    parser.add_argument("--catalog", type=pathlib.Path, default=DEFAULT_CATALOG)
    parser.add_argument("--metadata", type=pathlib.Path, default=DEFAULT_METADATA)
    parser.add_argument("--screenshots", action="store_true", default=True,
                        help="Parse screenshots from topic content")
    parser.add_argument("--no-screenshots", action="store_false",
                        dest="screenshots")
    parser.add_argument("--build-nro", action="store_true",
                        help="Run PC tests, Switch build, and refresh root pipensx.nro")
    args = parser.parse_args()

    input_xml = args.xml.resolve()
    dumper = args.dumper.resolve()
    if not input_xml.exists():
        raise SystemExit(f"XML dump not found: {input_xml}")
    dump_py = dumper / "dump.py"
    if not dump_py.exists():
        raise SystemExit(
            f"switch-dumps source checkout not found: {dumper}; "
            "clone https://github.com/bqio/switch-dumps.git there first"
        )

    args.catalog.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=str(args.catalog.parent)) as tmpdir:
        xml = materialize_xml(input_xml, tmpdir)
        tmp_catalog = pathlib.Path(tmpdir) / "catalog.json"
        cmd = [sys.executable, str(dump_py)]
        if args.screenshots:
            cmd.append("--screenshots")
        cmd.extend([str(xml), str(args.forum_id), str(tmp_catalog)])
        run(cmd, cwd=dumper)
        enrich_catalog(tmp_catalog, xml, args.forum_id)
        validate_catalog(tmp_catalog)
        shutil.copyfile(tmp_catalog, args.catalog)

    run([sys.executable, "tools/build_game_metadata.py",
         "--catalog", str(args.catalog),
         "--output", str(args.metadata)])
    validate_catalog(args.catalog)

    if args.build_nro:
        run(["make", "-f", "Makefile.pc", "-j4", "test"])
        run(["make", "-C", "build-switch", "-j4"])
        shutil.copyfile(ROOT / "build-switch" / "pipensx.nro",
                        ROOT / "pipensx.nro")
        run(["sha256sum", "pipensx.nro", "build-switch/pipensx.nro"])


if __name__ == "__main__":
    main()
