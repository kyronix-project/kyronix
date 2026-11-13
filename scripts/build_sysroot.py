#!/usr/bin/env python3
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

BASE = "https://deb.debian.org/debian/dists/bookworm/main/binary-amd64/"

SEEDS = [
    "libc6", "libgcc-s1", "libstdc++6", "zlib1g",
    "libwayland-client0", "libffi8", "libquadmath0",
    "libgtk-3-0", "libgtk-3-common",
    "fontconfig-config", "fonts-dejavu-core",
    "gsettings-desktop-schemas",
    "adwaita-icon-theme", "hicolor-icon-theme",
    "shared-mime-info", "librsvg2-common",
]

EXCLUDE = {"dconf-gsettings-backend", "gconf-gsettings-backend"}


def fetch(url, dest):
    last = None
    for attempt in range(5):
        try:
            with urllib.request.urlopen(url, timeout=120) as r, open(dest, "wb") as f:
                f.write(r.read())
            return
        except Exception as e:
            last = e
            time.sleep(2 * (attempt + 1))
    raise last


def parse_packages(path):
    pkgs = {}
    provides = {}
    txt = open(path, encoding="utf-8", errors="replace").read()
    for para in txt.split("\n\n"):
        m = re.search(r"^Package: (\S+)$", para, re.M)
        if not m:
            continue
        name = m.group(1)
        fn = re.search(r"^Filename: (\S+)$", para, re.M)
        deps = []
        for field in ("Depends", "Pre-Depends"):
            d = re.search(r"^%s: (.*)$" % field, para, re.M)
            if d:
                deps.append(d.group(1))
        prov = re.findall(r"^Provides: (.*)$", para, re.M)
        pkgs[name] = {
            "filename": fn.group(1).lstrip("./") if fn else None,
            "deps": ",".join(deps),
            "provides": prov,
        }
        for line in prov:
            for p in line.split(","):
                virt = p.strip().split(" ")[0]
                if virt:
                    provides.setdefault(virt, []).append(name)
    return pkgs, provides


def dep_groups(deps):
    groups = []
    for part in deps.split(","):
        part = part.strip()
        if not part:
            continue
        alts = []
        for alt in part.split("|"):
            m = re.match(r"([a-z0-9][a-z0-9+.-]*)", alt.strip())
            if m:
                alts.append(m.group(1))
        if alts:
            groups.append(alts)
    return groups


def resolve(pkgs, provides):
    seen = set()
    order = []
    queue = list(SEEDS)
    while queue:
        name = queue.pop(0)
        if name in seen or name in EXCLUDE:
            continue
        if name not in pkgs:
            cands = [c for c in provides.get(name, []) if c not in EXCLUDE]
            if not cands:
                continue
            name = cands[0]
            if name in seen:
                continue
        seen.add(name)
        order.append(name)
        for alts in dep_groups(pkgs[name]["deps"]):
            pick = None
            for alt in alts:
                if alt in EXCLUDE:
                    continue
                if alt in pkgs:
                    pick = alt
                    break
                cands = [c for c in provides.get(alt, []) if c not in EXCLUDE]
                if cands:
                    pick = cands[0]
                    break
            if pick:
                queue.append(pick)
    return order


def normalize_symlinks(stage):
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


def write_etc(stage):
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


def main():
    stage = sys.argv[1] if len(sys.argv) > 1 else "/work/stage"
    os.makedirs(stage, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="sysroot-dl-")

    idx_xz = os.path.join(tmp, "Packages.xz")
    idx = os.path.join(tmp, "Packages")
    fetch(BASE + "Packages.xz", idx_xz)
    subprocess.run(["xz", "-dc", idx_xz], stdout=open(idx, "wb"), check=True)

    pkgs, provides = parse_packages(idx)
    order = resolve(pkgs, provides)
    missing = [p for p in SEEDS if p not in pkgs]
    if missing:
        raise SystemExit(f"packages not found in index: {missing}")

    for pkg in order:
        rel = pkgs[pkg]["filename"]
        if not rel:
            continue
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

    normalize_symlinks(stage)
    write_etc(stage)
    print("STAGE_OK")


if __name__ == "__main__":
    main()
