# kair assets

Baked, tracked artifacts that `kair` (and the test suite that exercises it) needs
at runtime. They live here — in the repo — instead of `build/`, so that
`make clean` cannot destroy them.

Everything here is produced by `mk/assets.mk`. Nothing here is part of the Kyronix
OS proper: these are foreign (Debian/glibc) binaries that `kair` bind-mounts into
an AppImage sandbox.

## Contents

| Path | Size | What it is | Consumer |
|---|---|---|---|
| `sysroot/debian-sysroot.sqsh` | 59 MB | Debian bookworm glibc sysroot (libc, GTK3, fontconfig, wayland, …) | `kair` loop-mounts it at `/usr/lib/sysroots/debian.sqsh` |
| `fixtures/glibc/` | 3.0 MB | `hello-glibc` + its `libc.so.6`/`libm.so.6`/`ld-linux` | glibc test fixtures |
| `fixtures/real-app/` | 2.1 MB | Self-contained AppRun with its own `libs/` | wrapped into `real-app.img`, AppImage tests |
| `fixtures/wl-probe/` | 24 KB | Wayland client that resolves libwayland from the sysroot | Wayland tests |
| `apps/nvim.appimage` | 12 MB | Real upstream neovim AppImage | end-to-end AppImage test |
| `apps/galculator.appimage` | 3.4 MB | Real thin GTK3 AppImage | GUI smoke test |
| `apps/bash` | 2.3 MB | Static bash | script `AppRun` support in the sandbox |

## Rebuilding

These are baked, not built per-run. Normal `make` only *consumes* them.

```sh
make bake           # rebuild everything missing
make bake-sysroot   # just the sysroot
make bake-fixtures  # glibc + real-app + wl-probe
make bake-apps      # nvim + galculator + bash
make bake-check     # verify sha256 of what is here (fast, no build)
```

Baking needs `podman`/`docker` and network access to `deb.debian.org` and GitHub.
It is the only part of the build that does.

## How a rebuild is triggered

A baked artifact is stale — and gets rebuilt — when either changes:

1. **The recipe.** `KAIR_ASSET_REV` in `mk/assets.mk`, the `SYSROOT_SCRIPT_CMD` /
   `APPRUN_SCRIPT` / `WL_PROBE_SCRIPT` text, or the Debian image reference.
2. **The generator scripts or fixture sources.** `scripts/build_sysroot.py`,
   `scripts/build_sysroot_caches.sh`, `user/testrunner/fixtures/**`, or the
   `scripts/fetch_*.py` download pins (with their sha256).

For anything the above cannot see — most importantly **`bookworm` drifting in
place** under the same image tag — bump the revision by hand:

```sh
make KAIR_ASSET_REV=2 bake
```

After a rebake, refresh the checksums:

```sh
cd assets/kair && find . -type f -not -name SHA256SUMS -not -name README.md \
    | sort | xargs sha256sum > SHA256SUMS
```

## Why these are committed

The sysroot is assembled from a Debian `Packages.xz` index plus a transitive
`.deb` closure, unpacked (213 MB) and GTK cache-regenerated. It used to run on
every build and took minutes. Committed here it costs a `cp`.

The trade-off is repo size: ~82 MB. That is deliberate.
