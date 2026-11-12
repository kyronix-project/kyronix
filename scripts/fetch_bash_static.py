#!/usr/bin/env python3
"""Stage a statically linked bash for the AppImage sandbox. Emits STAGE_OK."""
import hashlib
import os
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

DEB_URL = ("https://deb.debian.org/debian/pool/main/b/bash/"
           "bash-static_5.2.15-2+b13_amd64.deb")
DEB_SHA256 = "f62e37fc6c2c14417f270323066b4c621db5dc151a26f7a1f58e3fc24111d1c8"
DEB_PATH = "bin/bash-static"
OUT_PATH = "bin/bash"


def fetch(url, dest):
    with urllib.request.urlopen(url, timeout=120) as r, open(dest, "wb") as f:
        f.write(r.read())


def verify(path, want):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    got = h.hexdigest()
    if got != want:
        raise SystemExit("checksum mismatch: got %s, want %s" % (got, want))


def main():
    stage = sys.argv[1] if len(sys.argv) > 1 else "/work/bash-stage"
    bin_dest = os.path.join(stage, OUT_PATH)
    os.makedirs(os.path.dirname(bin_dest), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="bash-dl-")

    deb = os.path.join(tmp, os.path.basename(DEB_URL))
    fetch(DEB_URL, deb)
    verify(deb, DEB_SHA256)

    adir = os.path.join(tmp, "ar")
    os.makedirs(adir, exist_ok=True)
    subprocess.run(["ar", "x", deb], cwd=adir, check=True)

    data = None
    for cand in ("data.tar.xz", "data.tar.gz", "data.tar.zst"):
        if os.path.exists(os.path.join(adir, cand)):
            data = os.path.join(adir, cand)
            break
    if not data:
        raise SystemExit("no data.tar.* in bash-static deb")

    mode = {".xz": "r:xz", ".gz": "r:gz", ".zst": "r:zstd"}[os.path.splitext(data)[1]]
    with tarfile.open(data, mode) as tf:
        member = None
        for m in tf.getmembers():
            if m.name.lstrip("./") == DEB_PATH:
                member = m
                break
        if member is None:
            raise SystemExit("%s not found in data archive" % DEB_PATH)
        src = tf.extractfile(member)
        if src is None:
            raise SystemExit("%s is not a regular file" % DEB_PATH)
        with open(bin_dest, "wb") as f:
            f.write(src.read())

    os.chmod(bin_dest, 0o755)
    print("STAGE_OK")


if __name__ == "__main__":
    main()
