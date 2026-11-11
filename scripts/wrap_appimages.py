#!/usr/bin/env python3
"""Wrap squashfs images with the ELF + 0x414902 type-2 AppImage prefix.

Usage: wrap_appimages.py <rootfs_dir> <gzip_sqsh> <xz_sqsh> <real_app_img>

Writes app.img, app_xz.img (and real_app.img when the source exists) into the
rootfs directory. The 4096-byte header carries ELF magic at [0..4) and the
AppImage type-2 marker AI\\x02 at [8..12), matching what appimage-run scans for.
"""
import os
import sys


def prefix():
    d = bytearray(4096)
    d[0:4] = b"\x7fELF"
    d[8:12] = b"AI\x02\x00"
    return bytes(d)


def wrap(dst, src):
    with open(dst, "wb") as out:
        out.write(prefix())
        with open(src, "rb") as inp:
            out.write(inp.read())


def main():
    rootfs, gz, xz, real = sys.argv[1:5]
    wrap(os.path.join(rootfs, "app.img"), gz)
    wrap(os.path.join(rootfs, "app_xz.img"), xz)
    if os.path.exists(real):
        wrap(os.path.join(rootfs, "real_app.img"), real)


if __name__ == "__main__":
    main()
