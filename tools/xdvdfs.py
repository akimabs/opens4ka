"""Extract a root file from an Xbox ISO, locally or directly from a phone over ADB.

Usage: xdvdfs.py <iso> <name> <out> [--device SERIAL] [--adb PATH]
With --device, the ISO path refers to the phone. The original ISO is never changed.
"""
import argparse
import hashlib
from pathlib import Path
import shlex
import struct
import subprocess
import sys

MAGIC = b"MICROSOFT*XBOX*MEDIA"
SECTOR = 2048
PARTITIONS = (0, 0x18300000, 0xFD90000, 0x2080000)


class PhoneImage:
    def __init__(self, path, serial, adb="adb"):
        self.path, self.serial, self.adb = path, serial, adb

    def read_at(self, offset, size):
        skip, leading = divmod(offset, SECTOR)
        count = (leading + size + SECTOR - 1) // SECTOR
        command = (f"dd if={shlex.quote(self.path)} bs={SECTOR} "
                   f"skip={skip} count={count} status=none")
        result = subprocess.run([self.adb, "-s", self.serial, "exec-out", command],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=90)
        if result.returncode:
            raise ValueError(result.stderr.decode(errors="replace").strip() or "ADB read failed")
        return result.stdout[leading:leading + size]


class LocalImage:
    def __init__(self, stream):
        self.stream = stream

    def read_at(self, offset, size):
        self.stream.seek(offset)
        return self.stream.read(size)


def extract(image, name):
    for base in PARTITIONS:
        descriptor = image.read_at(base + 32 * SECTOR, 28)
        if descriptor[:20] == MAGIC and len(descriptor) == 28:
            break
    else:
        raise ValueError("not an Xbox disc image")

    root_sector, root_size = struct.unpack_from("<II", descriptor, 20)
    if not 14 <= root_size <= 16 * 1024 * 1024:
        raise ValueError("invalid Xbox root directory size")
    table = image.read_at(base + root_sector * SECTOR, root_size)
    if len(table) != root_size:
        raise ValueError("truncated Xbox root directory")
    pending, visited = [0], set()
    while pending:
        offset = pending.pop() * 4
        if offset in visited or offset + 14 > len(table):
            raise ValueError("invalid Xbox directory entry")
        visited.add(offset)
        left, right, sector, size, attr, length = struct.unpack_from("<HHIIBB", table, offset)
        if left == 0xFFFF:
            continue
        if offset + 14 + length > len(table):
            raise ValueError("truncated Xbox filename")
        entry = table[offset + 14:offset + 14 + length].decode("latin-1")
        if entry.lower() == name.lower():
            if attr & 0x10:
                raise ValueError(f"{name} is a directory")
            data = image.read_at(base + sector * SECTOR, size)
            if len(data) != size:
                raise ValueError(f"{name} is truncated: expected {size} bytes, got {len(data)}")
            return data, base
        pending.extend(child for child in (right, left) if child)
    raise ValueError(f"{name} not found")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("iso")
    parser.add_argument("name")
    parser.add_argument("out", type=Path)
    parser.add_argument("--device", help="ADB serial; read only the needed blocks from this phone")
    parser.add_argument("--adb", default="adb", help="path to adb")
    parser.add_argument("--force", action="store_true", help="replace an existing extracted output")
    args = parser.parse_args()
    if args.out.exists() and not args.force:
        parser.error(f"{args.out} already exists; use --force only to replace it")
    try:
        if args.device:
            data, base = extract(PhoneImage(args.iso, args.device, args.adb), args.name)
        else:
            with open(args.iso, "rb") as stream:
                data, base = extract(LocalImage(stream), args.name)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("wb" if args.force else "xb") as output:
            output.write(data)
        print(f"wrote {args.out}: {len(data)} bytes, partition {base:#x}")
        print(f"SHA-1 {hashlib.sha1(data).hexdigest()}")
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"Extraction failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
