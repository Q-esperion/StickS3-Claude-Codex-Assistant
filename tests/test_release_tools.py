import hashlib
import json
import sys
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "helper"))

from release_tools import build_manifest, project_version, read_macro, write_release_bundle
from status_logic import clean_text, codex_phase_for_text


class ReleaseToolsTest(unittest.TestCase):
    def test_read_macro(self):
        self.assertRegex(read_macro(ROOT / "src" / "remote_ota_config.h", "APP_VERSION"), r"^\d+\.\d+\.\d+")

    def test_manifest_contains_hash_size_and_notes(self):
        fw = ROOT / "LICENSE"
        manifest = build_manifest("1.0.0", "https://example.com/firmware.bin", fw, "hello")
        self.assertEqual(manifest["sha256"], hashlib.sha256(fw.read_bytes()).hexdigest())
        self.assertEqual(manifest["size"], fw.stat().st_size)
        self.assertEqual(manifest["notes"], "hello")

    def test_write_release_bundle(self):
        expected = {"version": "1.0.0", "url": "https://example.com/firmware.bin", "sha256": "abc", "size": 3}
        with mock.patch("release_tools.shutil.copy2") as copy2, \
             mock.patch("release_tools.build_manifest", return_value=expected), \
             mock.patch("pathlib.Path.mkdir") as mkdir, \
             mock.patch("pathlib.Path.write_text") as write_text:
            manifest = write_release_bundle(
                ROOT / "LICENSE",
                ROOT / "dist" / "release",
                "1.0.0",
                "https://example.com/firmware.bin",
            )
        self.assertEqual(manifest, expected)
        mkdir.assert_called_once()
        copy2.assert_called_once()
        written = json.loads(write_text.call_args.args[0])
        self.assertEqual(written, expected)

    def test_project_version_has_default(self):
        self.assertRegex(project_version(ROOT), r"^\d+\.\d+\.\d+")


class StatusLogicTest(unittest.TestCase):
    def test_codex_phase_markers(self):
        self.assertEqual(codex_phase_for_text("\x01Uhello"), "thinking")
        self.assertEqual(codex_phase_for_text("\x01Cdone"), "idle")
        self.assertEqual(codex_phase_for_text("Bash(ls)"), "running")

    def test_clean_text(self):
        self.assertEqual(clean_text("  hi  "), "hi")
        self.assertEqual(clean_text(None), "")


if __name__ == "__main__":
    unittest.main()
