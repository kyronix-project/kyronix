#!/usr/bin/env python3
"""Stage a real AppImage for the end-to-end test. Emits STAGE_OK."""
import hashlib
import os
import sys
import urllib.request

URL = ("https://github.com/neovim/neovim/releases/download/"
       "v0.10.3/nvim.appimage")
SHA256 = "ce5f6462d4fa7150cc90c593955f66f7707ab5edc1d18f87413ea482e096c24e"

COMP_NAMES = {1: "gzip", 2: "lzma", 3: "lzo", 4: "xz", 5: "lz4", 6: "zstd"}


def fetch(url, dest):
    with urllib.request.urlopen(url, timeout=600) as r, open(dest, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)


def verify(path, want):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    got = h.hexdigest()
    if got != want:
        raise SystemExit("checksum mismatch: got %s, want %s" % (got, want))


def main():
    dest = sys.argv[1] if len(sys.argv) > 1 else "/work/nvim.appimage"
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"

    fetch(URL, tmp)
    verify(tmp, SHA256)
    os.replace(tmp, dest)

    with open(dest, "rb") as f:
        head = f.read(0x40)
        if head[:4] != b"\x7fELF":
            raise SystemExit("not an ELF AppImage")
        shoff = int.from_bytes(head[0x28:0x30], "little")
        shentsize = int.from_bytes(head[0x3a:0x3c], "little")
        shnum = int.from_bytes(head[0x3c:0x3e], "little")
        sqfs_off = shoff + shentsize * shnum
        f.seek(sqfs_off)
        if f.read(4) != b"hsqs":
            raise SystemExit("no squashfs at ELF section table end (%d)" % sqfs_off)
        f.seek(sqfs_off + 20)
        comp = int.from_bytes(f.read(2), "little")

    if comp not in (1, 4):
        raise SystemExit("squashfs compression %s is not supported by the kernel"
                         % COMP_NAMES.get(comp, comp))

    print("sqfs_off=%d comp=%s" % (sqfs_off, COMP_NAMES[comp]), file=sys.stderr)
    print("STAGE_OK")


if __name__ == "__main__":
    main()
