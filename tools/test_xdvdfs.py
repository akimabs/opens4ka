import io
import shlex
import struct
import subprocess
import unittest
from unittest.mock import patch

from xdvdfs import LocalImage, MAGIC, PhoneImage, SECTOR, extract


def fixture():
    data = bytearray(70 * SECTOR)
    data[32 * SECTOR:32 * SECTOR + 28] = MAGIC + struct.pack("<II", 40, SECTOR)
    name, payload = b"default.xbe", b"XBEH synthetic test content"
    entry = struct.pack("<HHIIBB", 0, 0, 60, len(payload), 0, len(name)) + name
    data[40 * SECTOR:40 * SECTOR + len(entry)] = entry
    data[60 * SECTOR:60 * SECTOR + len(payload)] = payload
    return data, payload


class ExtractTests(unittest.TestCase):
    def test_local_extraction_case_insensitive(self):
        data, payload = fixture()
        self.assertEqual(extract(LocalImage(io.BytesIO(data)), "DEFAULT.XBE"), (payload, 0))

    def test_missing_file(self):
        data, _ = fixture()
        with self.assertRaisesRegex(ValueError, "not found"):
            extract(LocalImage(io.BytesIO(data)), "missing.xbe")

    def test_invalid_image(self):
        with self.assertRaisesRegex(ValueError, "not an Xbox"):
            extract(LocalImage(io.BytesIO(b"not an ISO")), "default.xbe")

    def test_truncated_payload(self):
        data, _ = fixture()
        with self.assertRaisesRegex(ValueError, "truncated"):
            extract(LocalImage(io.BytesIO(data[:60 * SECTOR + 2])), "default.xbe")

    def test_directory_cycle(self):
        data, _ = fixture()
        struct.pack_into("<H", data, 40 * SECTOR, 16)
        struct.pack_into("<HHIIBB", data, 40 * SECTOR + 64, 16, 0, 0, 0, 0, 1)
        with self.assertRaisesRegex(ValueError, "invalid Xbox directory"):
            extract(LocalImage(io.BytesIO(data)), "missing.xbe")

    def test_phone_reads_only_requested_blocks_and_quotes_filename(self):
        path = "/storage/emulated/0/Download/Mortal Kombat/it's a game (USA).iso"
        payload = bytes(range(256)) * 16
        response = subprocess.CompletedProcess([], 0, payload, b"")
        with patch("xdvdfs.subprocess.run", return_value=response) as run:
            result = PhoneImage(path, "test-serial").read_at(2050, 2300)
        self.assertEqual(result, payload[2:2302])
        args = run.call_args.args[0]
        self.assertEqual(args[:4], ["adb", "-s", "test-serial", "exec-out"])
        self.assertIn("if=" + shlex.quote(path), args[4])
        self.assertIn("skip=1 count=2", args[4])
        self.assertNotIn("of=", args[4])

    def test_phone_failure_is_reported(self):
        response = subprocess.CompletedProcess([], 1, b"", b"Permission denied")
        with patch("xdvdfs.subprocess.run", return_value=response):
            with self.assertRaisesRegex(ValueError, "Permission denied"):
                PhoneImage("/missing.iso", "serial").read_at(0, 28)


if __name__ == "__main__":
    unittest.main()
