import base64
import contextlib
import io
import tempfile
import unittest
from pathlib import Path

import prep_fw


class PatchFirmwareTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.firmware = self.root / "main.bin"
        self.output = self.root / "patched.bin"
        self.key = bytes(range(prep_fw.KEY_LENGTH))

        image = bytearray(b"header")
        image.extend(prep_fw.KEY_MARKER)
        image.extend(
            bytes(prep_fw.MAX_KEYS * prep_fw.KEY_LENGTH - len(prep_fw.KEY_MARKER))
        )
        image.extend(b"middle")
        image.extend(prep_fw.GOOGLE_EID_MARKER)
        image.extend(b"tail")
        image.extend(prep_fw.CONFIG_MARKER)
        image.extend((30 * 1600).to_bytes(2, "little"))
        image.append(prep_fw.NETWORK_APPLE)
        self.firmware.write_bytes(image)

        self.keyfile = self.root / "device.keys"
        self.keyfile.write_text(
            "Advertisement key: " + base64.b64encode(self.key).decode() + "\n"
        )

    def tearDown(self):
        self.tempdir.cleanup()

    def patch(self, keyfiles, google_eid=None, interval=3):
        with contextlib.redirect_stdout(io.StringIO()):
            prep_fw.patch_fw(
                self.firmware,
                keyfiles,
                self.output,
                interval,
                google_eid,
            )
        return self.output.read_bytes()

    def config_values(self, image):
        start = image.index(prep_fw.CONFIG_MARKER) + len(prep_fw.CONFIG_MARKER)
        return int.from_bytes(image[start:start + 2], "little"), image[start + 2]

    def test_apple_mode_remains_backwards_compatible(self):
        image = self.patch([self.keyfile])
        key_start = image.index(self.key)

        self.assertEqual(image[key_start:key_start + prep_fw.KEY_LENGTH], self.key)
        self.assertEqual(self.config_values(image), (3 * 1600, prep_fw.NETWORK_APPLE))
        self.assertIn(prep_fw.GOOGLE_EID_MARKER, image)

    def test_apple_address_uses_only_the_first_six_apple_key_bytes(self):
        key = bytes([0x12, 0x23, 0x34, 0x45, 0x56, 0x67]) + bytes(22)

        self.assertEqual(
            prep_fw.apple_advertising_address(key),
            bytes([0xD2, 0x23, 0x34, 0x45, 0x56, 0x67]),
        )

    def test_google_only_mode_patches_eid(self):
        eid = bytes(range(20, 40))
        image = self.patch([], eid.hex(), interval=5)

        self.assertIn(eid, image)
        self.assertIn(prep_fw.KEY_MARKER, image)
        self.assertEqual(self.config_values(image), (5 * 1600, prep_fw.NETWORK_GOOGLE))

    def test_dual_mode_patches_both_networks(self):
        eid = bytes(range(40, 60))
        image = self.patch([self.keyfile], eid)

        self.assertIn(self.key, image)
        self.assertIn(eid, image)
        self.assertEqual(self.config_values(image), (3 * 1600, prep_fw.NETWORK_DUAL))

    def test_rejects_invalid_google_eid(self):
        with self.assertRaisesRegex(ValueError, "expected 20"):
            self.patch([], "0011")

    def test_requires_at_least_one_network_credential(self):
        with self.assertRaisesRegex(ValueError, "at least one Apple key"):
            self.patch([])


if __name__ == "__main__":
    unittest.main()
