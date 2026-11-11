#!/usr/bin/env python3
"""Stage G3: build a minimal glibc sysroot from Debian bookworm runtime debs.

Runs inside the Alpine kyronix-build container (no apt/dpkg): it downloads the
main/binary-amd64 Packages index, resolves the exact Pool filenames for a fixed
package set, fetches each .deb with urllib, unpacks data.tar.* via tar, then
normalizes SONAME symlinks and writes the pkg-repo style layout. Emits STAGE_OK
on success so the make recipe can gate on it; any network/parse failure raises
and the caller degrades to a warning.
"""
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

BASE = "https://deb.debian.org/debian/dists/bookworm/main/binary-amd64/"
PKGS = ["libc6", "libgcc-s1", "libstdc++6", "zlib1g"]


def fetch(url, dest):
    with urllib.request.urlopen(url, timeout=120) as r, open(dest, "wb") as f:
        f.write(r.read())


def parse_packages(path):
    """Return {package_name: Filename} from an uncompressed Packages file."""
    txt = open(path, encoding="utf-8", errors="replace").read()
    out = {}
    for para in txt.split("\n\n"):
        m = re.search(r"^Package: (\S+)$", para, re.M)
        if not m or m.group(1) not in PKGS:
            continue
        f = re.search(r"^Filename: (\S+)$", para, re.M)
        if f:
            out[m.group(1)] = f.group(1).lstrip("./")
    return out


def main():
    stage = sys.argv[1] if len(sys.argv) > 1 else "/work/stage"
    os.makedirs(stage, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="sysroot-dl-")

    # Fetch and decompress the package index.
    idx_xz = os.path.join(tmp, "Packages.xz")
    idx = os.path.join(tmp, "Packages")
    fetch(BASE + "Packages.xz", idx_xz)
    subprocess.run(["xz", "-dc", idx_xz], stdout=open(idx, "wb"), check=True)

    names = parse_packages(idx)
    missing = [p for p in PKGS if p not in names]
    if missing:
        raise SystemExit(f"packages not found in index: {missing}")

    # Download + unpack each deb's data archive.
    for pkg in PKGS:
        rel = names[pkg]
        deb = os.path.join(tmp, os.path.basename(rel))
        fetch("https://deb.debian.org/debian/" + rel, deb)
        adir = tempfile.mkdtemp(prefix="deb-", dir=tmp)
        subprocess.run(["ar", "x", deb], cwd=adir, check=True)
        data = None
        for cand in ("data.tar.xz", "data.tar.gz", "data.tar.zst"):
            if os.path.exists(os.path.join(adir, cand)):
                data = os.path.join(adir, cand)
                break
        if not data:
            raise SystemExit(f"{pkg}: no data.tar.* found")
        mode = {".xz": "r:xz", ".gz": "r:gz", ".zst": "r:zstd"}[os.path.splitext(data)[1]]
        with tarfile.open(data, mode) as tf:
            tf.extractall(stage)
        print(f"fetched {pkg}", file=sys.stderr)

    # Normalize SONAME symlinks. Debian ships either the full-revision file
    # (libstdc++.so.6.0.30) or the major-version file (libc.so.6) as the real
    # object; dev packages normally add the bare .so but runtime-only debs may
    # not, so we synthesize any missing link in the chain .so -> .so.MAJ -> rev.
    for d in ("lib/x86_64-linux-gnu", "usr/lib/x86_64-linux-gnu"):
        full = os.path.join(stage, d)
        if not os.path.isdir(full):
            continue
        present = set(os.listdir(full))
        for fn in list(present):
            parts = fn.split(".so.")
            if len(parts) != 2 or not parts[1][:1].isdigit():
                continue
            stem = parts[0] + ".so"
            maj = parts[1].split(".")[0]
            soname = stem + "." + maj
            # Determine the real (non-symlink) file at the tip of the chain.
            real = fn
            seen = 0
            while os.path.islink(os.path.join(full, real)) and seen < 8:
                tgt = os.readlink(os.path.join(full, real))
                if tgt not in present:
                    break
                real = tgt
                seen += 1
            for link, target in ((soname, real), (stem, soname)):
                if link == fn:
                    continue
                lp = os.path.join(full, link)
                if link not in present and not os.path.lexists(lp):
                    try:
                        os.symlink(target, lp)
                        present.add(link)
                    except OSError:
                        pass

    # Minimal /etc for NSS + timezone.
    etc = os.path.join(stage, "etc")
    os.makedirs(etc, exist_ok=True)
    with open(os.path.join(etc, "nsswitch.conf"), "w") as f:
        f.write("hosts: files dns\npasswd: files\nshadow: files\n"
                "group: files\ninitgroups: files\nservices: files\nprotocols: files\n"
                "networks: files\nethers: files\nrpc: files\n")
    with open(os.path.join(etc, "passwd"), "w") as f:
        f.write("root:x:0:0:root:/root:/bin/sh\n"
                "nobody:x:65534:65534:nobody:/:/usr/sbin/nologin\n")
    with open(os.path.join(etc, "group"), "w") as f:
        f.write("root:x:0:\nnogroup:x:65534:\n")
    lt = os.path.join(etc, "localtime")
    if not os.path.lexists(lt):
        try:
            os.symlink("/usr/share/zoneinfo/UTC", lt)
        except OSError:
            pass
    os.makedirs(os.path.join(stage, "lib64"), exist_ok=True)
    os.makedirs(os.path.join(stage, "usr/lib/locale"), exist_ok=True)

    # Best-effort ld.so.cache generation (skip silently if ldconfig absent).
    ldc = shutil_which("ldconfig")
    if ldc:
        subprocess.run([ldc, "-N", "-r", stage], check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print("STAGE_OK")


def shutil_which(name):
    for p in os.environ.get("PATH", "").split(":"):
        c = os.path.join(p, name)
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


if __name__ == "__main__":
    main()
