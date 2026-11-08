#!/usr/bin/env python3
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "mirror_langegen", Path(__file__).resolve().parents[1] / "scripts/mirror_langegen.py")
mirror = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mirror)


class MirrorTests(unittest.TestCase):
    def test_rejects_invalid_catalogs_and_diffs(self):
        for payload in (b"[]", b"{}", b"[{}]", b"<html>error</html>",
                        b'[{"title":"x","magnet":"https://example.com"}]'):
            with self.assertRaises(ValueError):
                mirror.validate("RU_catalog.json", payload)
        with self.assertRaises(ValueError):
            mirror.validate("catalog_diff.json", b'{"version":1}')

    def test_accepts_upstream_shape(self):
        mirror.validate("EN_catalog.json", b'[{"title":"x","magnet":"magnet:?xt=x"}]')
        mirror.validate("catalog_diff.json", json.dumps({
            "version": 1, "added_or_updated": {}, "deleted_topic_ids": []}).encode())

    def test_manifest_is_published_last(self):
        manifest = {"schemaVersion": 1, "files": {
            "RU_catalog.json": {"sha256": "a" * 64, "bytes": 10},
            "EN_catalog.json": {"sha256": "b" * 64, "bytes": 11}}}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            with patch.object(mirror, "download", return_value=None), \
                 patch.object(mirror, "upload") as upload:
                mirror.publish(output, manifest)
                self.assertEqual(upload.call_args_list[-1].args,
                                 (output / "manifest.json", "latest"))
                self.assertEqual(upload.call_count, 3)

    def test_failed_snapshot_upload_preserves_manifest(self):
        manifest = {"files": {"RU_catalog.json": {"sha256": "a" * 64}}}
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(mirror, "download", return_value=None), \
             patch.object(mirror, "upload", side_effect=RuntimeError("upload failed")) as upload:
            with self.assertRaises(RuntimeError):
                mirror.publish(Path(directory), manifest)
            self.assertEqual(upload.call_count, 1)
            self.assertNotEqual(upload.call_args.args[1], "latest")

    def test_unchanged_manifest_is_not_reuploaded(self):
        manifest = {"schemaVersion": 1, "files": {}}
        with patch.object(mirror, "download", return_value=json.dumps(manifest).encode()), \
             patch.object(mirror, "upload") as upload:
            mirror.publish(Path("unused"), manifest)
            upload.assert_not_called()


if __name__ == "__main__":
    unittest.main()
