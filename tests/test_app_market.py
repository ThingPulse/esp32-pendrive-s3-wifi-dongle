import hashlib
import json
import tempfile
import unittest
from pathlib import Path

from scripts import app_market


class AppMarketTests(unittest.TestCase):
    def test_version_normalization_and_prerelease(self):
        self.assertEqual(app_market.normalize_version("v1.2.0-rc.1"), "1.2.0-rc.1")
        self.assertTrue(app_market.is_prerelease("v1.2.0-beta.1"))
        self.assertFalse(app_market.is_prerelease("v1.2.0"))
        with self.assertRaises(ValueError):
            app_market.normalize_version("release-1")

    def test_sha256(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "asset.bin"
            path.write_bytes(b"firmware")
            self.assertEqual(app_market.digest(path), hashlib.sha256(b"firmware").hexdigest())

    def fixture(self, directory: Path, missing=None, offset_override=None):
        build = directory / "build"
        flash_files = {}
        for index, (_, (relative, offset)) in enumerate(app_market.EXPECTED.items()):
            flash_files[hex(offset_override.get(relative, offset) if offset_override else offset)] = relative
            if relative != missing:
                path = build / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(bytes([index + 1]) * 32)
        build.mkdir(parents=True, exist_ok=True)
        (build / "flasher_args.json").write_text(json.dumps({"flash_files": flash_files}))
        return build

    def test_artifact_discovery_and_offsets(self):
        with tempfile.TemporaryDirectory() as directory:
            parts = app_market.discover(self.fixture(Path(directory)))
            self.assertEqual([part["offset"] for part in parts], [0, 0x8000, 0xD000, 0x10000])
            self.assertEqual([part["asset"] for part in parts], ["bootloader.bin", "partition-table.bin", "ota_data_initial.bin", "usb_dongle.bin"])

    def test_missing_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(FileNotFoundError):
                app_market.discover(self.fixture(Path(directory), "usb_dongle.bin"))

    def test_wrong_offset(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                app_market.discover(self.fixture(Path(directory), offset_override={"usb_dongle.bin": 0x20000}))

    def test_overlapping_ranges(self):
        with self.assertRaises(ValueError):
            app_market.check_ranges([
                {"name": "one", "offset": 0, "size": 20},
                {"name": "two", "offset": 10, "size": 20},
            ])

    def test_manifest_structure_and_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "_static").mkdir()
            (root / "_static/ESP32-S3.png").write_bytes(b"png-test-data")
            manifest_path = app_market.generate(root, self.fixture(root), root / "release", "v1.2.0")
            manifest = app_market.validate(manifest_path)
            self.assertEqual(manifest["app"]["supportedDevices"], ["tp-pendrive-s3"])
            self.assertEqual(manifest["release"]["version"], "1.2.0")
            self.assertEqual(len(manifest["release"]["partitions"]), 4)
            self.assertEqual(manifest["app"]["icon"]["asset"], "ESP32-S3.png")
            self.assertEqual(len(manifest["app"]["icon"]["sha256"]), 64)

    def test_missing_icon(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(FileNotFoundError):
                app_market.generate(root, self.fixture(root), root / "release", "v1.2.0")


if __name__ == "__main__":
    unittest.main()
