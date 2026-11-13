#!/usr/bin/env python3
"""Offline tests for host/webui/device.py: validation and path handling only."""
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "webui"))
import device  # noqa: E402
import lights  # noqa: E402
import server  # noqa: E402
import settings as smcpad_settings  # noqa: E402

GOOD_APP = os.path.join(device.ROOT, "work", "sketch", "MidiFighterSpectra",
                        "MidiFighterSpectra.bin")
GOOD_AREA = os.path.join(device.ROOT, "work", "flash", "fw_area.bin")
RECOVERY = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "webui", "recovery")


class ValidateTest(unittest.TestCase):
    def test_crc16_xmodem_check_value(self):
        self.assertEqual(device.crc16(b"123456789"), 0x31C3)

    def test_known_good_app_area(self):
        data = Path(GOOD_APP).read_bytes()
        self.assertEqual(device.validate_app_area(data, GOOD_APP), (True, ""))
        self.assertEqual(struct.unpack_from("<I", data, device.DIR_SIZE)[0], len(data))

    def test_packaged_app_area(self):
        data = Path(GOOD_AREA).read_bytes()
        self.assertEqual(device.validate_app_area(data, GOOD_AREA), (True, ""))

    def test_rejects_fwsc_name(self):
        data = Path(GOOD_APP).read_bytes()
        ok, why = device.validate_app_area(data, "SMC-PAD_005.fwsc")
        self.assertFalse(ok)
        self.assertIn(".fwsc", why)

    def test_rejects_full_dump(self):
        data = b"\xff" * device.FLASH_SIZE
        ok, why = device.validate_app_area(data, "flash_dump.bin")
        self.assertFalse(ok)
        self.assertIn("full raw flash dump", why)

    def test_rejects_corrupt_header(self):
        data = bytearray(Path(GOOD_APP).read_bytes())
        data[10] ^= 0xFF
        ok, why = device.validate_app_area(bytes(data), "x.bin")
        self.assertFalse(ok)
        self.assertIn("header CRC", why)

    def test_rejects_truncated_area(self):
        data = Path(GOOD_APP).read_bytes()[:-4096]
        ok, why = device.validate_app_area(data, "x.bin")
        self.assertFalse(ok)
        self.assertIn("declares more data", why)

    def test_rejects_bad_area_crc(self):
        data = bytearray(Path(GOOD_APP).read_bytes())
        data[0x100] ^= 0xFF
        ok, why = device.validate_app_area(bytes(data), "x.bin")
        self.assertFalse(ok)
        self.assertIn("CRC mismatch", why)

    def test_rejects_short_file(self):
        ok, why = device.validate_app_area(b"\x00" * 16, "x.bin")
        self.assertFalse(ok)


class RecoveryAssetsTest(unittest.TestCase):
    """The Debrick tab's hosted images, pinned in recovery/manifest.json."""

    PINNED = {
        "app.bin": (149128, "b11926ea0d01b9ad524d559d6e8b067c913bd22fb518c54bacac8079d638b824"),
        "boot_update.bin": (28672, "b5a5c951158ce4cdfd2005aabf8236706c19325f95b1700ab653da96e1585ae0"),
    }

    def test_manifest_matches_the_pins(self):
        manifest = json.loads(Path(RECOVERY, "manifest.json").read_text())
        self.assertEqual(set(manifest), set(self.PINNED))
        for name, (size, sha) in self.PINNED.items():
            self.assertEqual(manifest[name]["size"], size, name)
            self.assertEqual(manifest[name]["sha256"], sha, name)

    def test_assets_are_the_pinned_bytes(self):
        for name, (size, sha) in self.PINNED.items():
            data = Path(RECOVERY, name).read_bytes()
            self.assertEqual(len(data), size, name)
            self.assertEqual(hashlib.sha256(data).hexdigest(), sha, name)

    def test_app_asset_validates_as_an_app_area(self):
        data = Path(RECOVERY, "app.bin").read_bytes()
        self.assertEqual(device.validate_app_area(data, "recovery/app.bin"), (True, ""))

    def test_boot_package_size_is_accepted_by_the_page(self):
        data = Path(RECOVERY, "boot_update.bin").read_bytes()
        self.assertTrue(0x1000 <= len(data) <= 0xC000)


class SafePathTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, "backups")
        os.makedirs(self.root)
        Path(self.root, "app_20260101-000000.bin").touch()
        self.patch = mock.patch.object(device, "BACKUPS", self.root)
        self.patch.start()

    def tearDown(self):
        self.patch.stop()
        self.tmp.cleanup()

    def test_accepts_plain_name(self):
        path = device.safe_path("backups", "app_20260101-000000.bin")
        self.assertEqual(path, os.path.realpath(os.path.join(self.root,
                                                             "app_20260101-000000.bin")))

    def test_rejects_traversal(self):
        for name in ("../x.bin", "..%2Fx.bin", "/etc/passwd", "a/../b.bin",
                     "app.bin\x00", "x.txt", ".hidden.bin"):
            self.assertIsNone(device.safe_path("backups", name), name)

    def test_rejects_symlink_escape(self):
        outside = os.path.join(self.tmp.name, "outside.bin")
        Path(outside).touch()
        os.symlink(outside, os.path.join(self.root, "link.bin"))
        self.assertIsNone(device.safe_path("backups", "link.bin"))

    def test_missing_file(self):
        self.assertIsNone(device.safe_path("backups", "nope.bin"))

    def test_unknown_root(self):
        self.assertIsNone(device.safe_path("etc", "x.bin"))


class SettingsTest(unittest.TestCase):
    def test_defaults(self):
        clean, errors = smcpad_settings.validate({})
        self.assertEqual(errors, {})
        self.assertEqual(clean, smcpad_settings.DEFAULTS)

    def test_bool_strings(self):
        self.assertFalse(smcpad_settings.validate({"spark": "0"})[0]["spark"])
        self.assertTrue(smcpad_settings.validate({"spark": "true"})[0]["spark"])

    def test_rejects_out_of_range(self):
        for key, value in (("base_ch", 0), ("base_ch", 15), ("key_velocity", 128),
                           ("sleep_min", 241), ("side_mode", 3), ("active_b0", 0x1000000),
                           ("side_cc_release", 200)):
            clean, errors = smcpad_settings.validate({key: value})
            self.assertIn(key, errors, key)

    def test_defines(self):
        d = smcpad_settings.defines(smcpad_settings.DEFAULTS)
        self.assertIn("-DSPECTRA_BASE_CH=3", d)
        self.assertIn("-DSPECTRA_KEY_VELOCITY=127", d)
        self.assertIn("-DSPECTRA_ACTIVE_B0=0xFFFFFFu", d)
        self.assertIn("-DSPECTRA_INACTIVE_B3=0x000000u", d)

    def test_save_load_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "settings.json")
            with mock.patch.object(smcpad_settings, "PATH", path):
                values = dict(smcpad_settings.DEFAULTS, base_ch=7, active_b2=0x00FF00,
                              momentary_cc=True)
                saved = smcpad_settings.save(values)
                self.assertEqual(saved["base_ch"], 7)
                self.assertEqual(smcpad_settings.load(), saved)


class LightsTest(unittest.TestCase):
    def test_colour_messages_follow_the_bank_and_channel(self):
        self.assertEqual(lights.build("color", 3, 0, [0], 67), [b"\x92\x24\x43"])
        self.assertEqual(lights.build("color", 3, 1, [0], 67), [b"\x92\x34\x43"])
        self.assertEqual(lights.build("color", 3, 0, [15], 1), [b"\x92\x33\x01"])
        self.assertEqual(lights.build("color", 5, 0, [0], 0), [b"\x94\x24\x00"])

    def test_animation_and_bank_messages(self):
        self.assertEqual(lights.build("anim", 3, 0, [1], 18), [b"\x93\x25\x12"])
        self.assertEqual(lights.build("bank", 3, 0, [], 2), [b"\x93\x02\x7f"])
        self.assertEqual(lights.build("force", 3, 0, [2], 0), [b"\x92\x26\x7f"])

    def test_rejects_bad_input(self):
        for args in (("color", 3, 0, [0], 121), ("anim", 3, 0, [0], 50),
                     ("color", 3, 0, [16], 1), ("color", 3, 0, [], 1),
                     ("color", 15, 0, [0], 1), ("nope", 3, 0, [0], 1)):
            with self.assertRaises(ValueError, msg=str(args)):
                lights.build(*args)

    def test_velocity_to_palette(self):
        self.assertEqual(lights.rgb_for_velocity(67), 0x00FFFF)
        self.assertEqual(lights.rgb_for_velocity(0), None)
        self.assertEqual(lights.rgb_for_velocity(121), None)

    def test_consume_and_hits(self):
        buf = bytearray(b"\x92\x24\x43\x92\x24\x00\xf8")
        events = lights._consume(buf, 3)
        self.assertEqual(events, [(0x92, 0x24, 0x43), (0x92, 0x24, 0x00)])
        self.assertEqual(buf, b"")
        self.assertEqual(lights._hits(events, 3), [(0, True), (0, False)])


class OriginTest(unittest.TestCase):
    def test_pages_origin_is_allowed(self):
        self.assertTrue(server.origin_allowed("https://drcrecoverydata.github.io", 8765))

    def test_local_origins_are_allowed(self):
        self.assertTrue(server.origin_allowed("http://127.0.0.1:8765", 8765))
        self.assertTrue(server.origin_allowed("http://localhost:8765", 8765))
        self.assertTrue(server.origin_allowed(None, 8765))

    def test_other_origins_are_refused(self):
        for origin in ("https://evil.example", "http://127.0.0.1:9999",
                       "https://drcrecoverydata.github.io.evil.example"):
            self.assertFalse(server.origin_allowed(origin, 8765), origin)

    def test_pages_origin_is_configurable(self):
        import importlib
        with mock.patch.dict(os.environ, {"SMCPAD_WEBUI_ORIGINS": "https://example.test"}):
            self.assertIn("https://example.test",
                          importlib.reload(server).allowed_origins(8765))
        importlib.reload(server)  # restore the default for the other tests


if __name__ == "__main__":
    unittest.main()
