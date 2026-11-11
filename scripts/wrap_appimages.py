#!/usr/bin/env python3
"""Usage:
    wrap_appimages.py <rootfs_dir> <gzip_sqsh> <xz_sqsh>
    wrap_appimages.py --single <sqsh> <out>
"""
import os
import struct
import sys

SHENTSIZE = 64
SHNUM = 3
PAD = 100

SQFS_MAGIC = b"hsqs"


def prefix():
    total = 4096 + PAD
    shoff = total - SHENTSIZE * SHNUM

    d = bytearray(total)
    d[0:4] = b"\x7fELF"
    d[4] = 2
    d[5] = 1
    d[6] = 1
    d[8:12] = b"AI\x02\x00"
    struct.pack_into("<H", d, 0x10, 2)
    struct.pack_into("<H", d, 0x12, 0x3E)
    struct.pack_into("<I", d, 0x14, 1)
    struct.pack_into("<Q", d, 0x28, shoff)
    struct.pack_into("<H", d, 0x3A, SHENTSIZE)
    struct.pack_into("<H", d, 0x3C, SHNUM)

    assert shoff + SHENTSIZE * SHNUM == total
    assert total % 512 != 0 and total % 4096 != 0, "fixture prefix must be unaligned"
    return bytes(d)


def payload_offset():
    return len(prefix())


def wrap(dst, src):
    p = prefix()
    with open(src, "rb") as inp:
        body = inp.read()
    assert body[:4] == SQFS_MAGIC, f"{src}: not a squashfs image"
    with open(dst, "wb") as out:
        out.write(p)
        out.write(body)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--single":
        wrap(sys.argv[3], sys.argv[2])
        return

    if len(sys.argv) != 4:
        sys.exit(__doc__)
    rootfs, gz, xz = sys.argv[1:4]
    wrap(os.path.join(rootfs, "app.img"), gz)
    wrap(os.path.join(rootfs, "app_xz.img"), xz)


if __name__ == "__main__":
    main()
