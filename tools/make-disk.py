#!/usr/bin/env python3
"""Build a disk image holding an ext2 file system, for testing Vexa's drivers.

Usage: tools/make-disk.py OUTPUT SIZE_MIB {gpt,mbr,none} CONTENT_DIR

The file system is made by mke2fs (from e2fsprogs) and filled from
CONTENT_DIR. With gpt or mbr it sits in a single partition starting at 1 MiB;
with none it fills the whole disk. No root access or loop devices needed.
"""
import os
import struct
import subprocess
import sys
import uuid
import zlib

SECTOR = 512
PARTITION_START = 1024 * 1024
LINUX_FILESYSTEM_GUID = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4")


def gpt(size, part_first, part_last):
    """A protective MBR, a primary GPT and a backup GPT with one partition."""
    sectors = size // SECTOR
    entries = bytearray(128 * 128)
    entries[0:128] = struct.pack(
        "<16s16sQQQ72s", LINUX_FILESYSTEM_GUID.bytes_le, uuid.uuid4().bytes_le,
        part_first, part_last, 0, "vexa-test".encode("utf-16-le").ljust(72, b"\0"))
    entries_crc = zlib.crc32(entries)

    def header(current, backup, entries_lba):
        fields = [b"EFI PART", 0x00010000, 92, 0, 0, current, backup, 34, sectors - 34,
                  uuid.uuid4().bytes_le, entries_lba, 128, 128, entries_crc]
        raw = struct.pack("<8sIIIIQQQQ16sQIII", *fields)
        fields[3] = zlib.crc32(raw)
        return struct.pack("<8sIIIIQQQQ16sQIII", *fields).ljust(SECTOR, b"\0")

    mbr = bytearray(SECTOR)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\0\x02\0", 0xEE, b"\xff\xff\xff", 1,
                               min(sectors - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    primary = bytes(mbr) + header(1, sectors - 1, 2) + entries
    backup = entries + header(sectors - 1, 1, sectors - 33)
    return primary, backup


def mbr(size, part_first, part_last):
    table = bytearray(SECTOR)
    table[446:462] = struct.pack("<B3sB3sII", 0, b"\0\x02\0", 0x83, b"\xff\xff\xff",
                                 part_first, part_last - part_first + 1)
    table[510:512] = b"\x55\xaa"
    return bytes(table), b""


def main():
    output, size_mib, layout, content = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    size = size_mib * 1024 * 1024
    with open(output, "wb") as f:
        f.truncate(size)

    if layout == "none":
        offset, fs_size = 0, size
    else:
        offset = PARTITION_START
        # Leave 33 sectors at the end for the backup GPT.
        fs_size = (size - offset - 34 * SECTOR) // 4096 * 4096
        first, last = offset // SECTOR, (offset + fs_size) // SECTOR - 1
        start, end = (gpt if layout == "gpt" else mbr)(size, first, last)
        with open(output, "r+b") as f:
            f.write(start)
            if end:
                f.seek(size - len(end))
                f.write(end)

    subprocess.run(["mke2fs", "-q", "-F", "-t", "ext2", "-b", "1024", "-L", "vexa-test",
                    "-E", f"offset={offset},root_owner=0:0", "-d", content, output,
                    str(fs_size // 1024)], check=True,
                   env=dict(os.environ, E2FSPROGS_FAKE_TIME="0"))


if __name__ == "__main__":
    main()
