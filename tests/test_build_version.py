import importlib.util
from pathlib import Path
import os
import subprocess
import unittest
import tempfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("build_version", Path(__file__).resolve().parents[1] / "scripts/version.py")
version = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version)


class BuildVersionTests(unittest.TestCase):
    def test_release_override(self):
        self.assertEqual(version.resolve_version("v2.3.4"), "2.3.4")
        with patch.dict(os.environ, {"PIPENSX_VERSION": "3.4.5"}, clear=True):
            self.assertEqual(version.resolve_version(), "3.4.5")

    def test_git_builds(self):
        cases = {
            "1.6.6-0-gabcdef": "1.6.6",
            "1.6.6-0-gabcdef-dirty": "1.6.6-dev.0+gabcdef.dirty",
            "1.6.6-12-gabcdef": "1.6.6-dev.12+gabcdef",
            "v2.3.4-0-gabcdef": "2.3.4",
        }
        with patch.dict(os.environ, {}, clear=True):
            for description, expected in cases.items():
                with patch.object(version.subprocess, "check_output", return_value=description):
                    self.assertEqual(version.resolve_version(), expected)

    def test_archive_fallback(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(version.subprocess, "check_output", side_effect=OSError):
            self.assertEqual(version.resolve_version(), (version.ROOT / "VERSION").read_text().strip())

    def test_rejects_invalid_override(self):
        for value in ["0.4", "1.2.3\"", "1.2.3\n", "01.2.3", "1.2.3" + "a" * 65]:
            with self.assertRaises(ValueError):
                version.resolve_version(value)

    def test_numeric_projection(self):
        result = subprocess.check_output(["python3", str(version.ROOT / "scripts/version.py"), "--version", "2.3.4-dev.2+gabcdef", "--numeric"], text=True)
        self.assertEqual(result.strip(), "2.3.4")

    def test_make_rebuilds_when_version_changes(self):
        with tempfile.TemporaryDirectory(prefix="pipensx-version-") as directory:
            target = f"{directory}/tests/test_client_identity"
            for value in ["2.3.4", "v3.4.5", "2.3.4"]:
                subprocess.run(["make", "--no-print-directory", "-f", "Makefile.pc",
                    f"BUILD_DIR={directory}", f"PIPENSX_VERSION={value}", target],
                    cwd=version.ROOT, check=True, stdout=subprocess.DEVNULL)
                result = subprocess.check_output([target], text=True)
                self.assertIn("pipensx/" + value.lstrip("v"), result)


if __name__ == "__main__":
    unittest.main()
