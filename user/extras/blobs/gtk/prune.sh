#!/bin/sh
# Prune superseded versioned libraries from /usr/lib/wayland.
#
# After merging gtk/usr/lib over the wayland blob libs, both the old
# (wayland-blob) and new (GTK) versioned real files can coexist, e.g.
#     libglib-2.0.so.0.8000.5   (wayland blobs, glib 2.80)
#     libglib-2.0.so.0.8200.5   (GTK, glib 2.82)
# The .so.N SONAME symlink now points at the newest. Keep only the newest
# versioned real file per SONAME (library major version); drop the older
# dead weights.
#
# We iterate over the SONAME symlinks (lib*.so.N) and only consider the
# regular files that belong to each one (its name plus more numeric parts).
# Symlinks themselves are never touched.

ROOTFS=$1
DIR=${ROOTFS}/usr/lib/wayland

[ -d "$DIR" ] || exit 0

for so in "$DIR"/lib*.so.[0-9]*; do
    [ -L "$so" ] || continue
    base=$(basename "$so")
    keep=""
    for f in "$DIR"/"$base".[0-9]*; do
        [ -f "$f" ] && [ ! -L "$f" ] || continue
        # strip dir; remaining tail has the version digits
        tail=${f##*/}
        tail=${tail#"$base".}
        if [ -z "$keep" ]; then
            keep=$tail
        else
            # pad to "N.N.N", then compare numerically
            nh() { printf '%s' "$1" | awk -F. '{ printf "%09d%09d", $1, $2 }'; }
            if [ "$(nh "$tail")" \> "$(nh "$keep")" ]; then
                keep=$tail
            fi
        fi
    done
    [ -n "$keep" ] || continue
    for f in "$DIR"/"$base".[0-9]*; do
        [ -f "$f" ] && [ ! -L "$f" ] || continue
        tail=${f##*/}
        tail=${tail#"$base".}
        if [ "$tail" != "$keep" ]; then
            rm -f -- "$f"
        fi
    done
done
exit 0