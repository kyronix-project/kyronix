#!/bin/sh
# Generate GTK/GLib runtime caches inside an already-unpacked glibc sysroot.
# Runs inside a Debian bookworm container: the staged tree lives at $1 (an
# absolute path under the bind-mounted source tree). Emits CACHE_OK on success.
set -e

S="$1"
if [ -z "$S" ] || [ ! -d "$S" ]; then
    echo "usage: $0 <sysroot-stage>" >&2
    exit 2
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update >/dev/null 2>&1
apt-get install -y --no-install-recommends \
    libglib2.0-bin libgdk-pixbuf-2.0-0 libgtk-3-bin shared-mime-info fontconfig \
    >/dev/null 2>&1

sed_i() { sed -i "s|$S||g" "$1"; }

# GLib settings schemas.
SCHEMAS="$S/usr/share/glib-2.0/schemas"
if [ -d "$SCHEMAS" ]; then
    glib-compile-schemas "$SCHEMAS" || true
fi

# GdkPixbuf loaders.
LOADERS="$S/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders"
CACHE="$S/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache"
if [ -d "$LOADERS" ]; then
    QL=""
    for c in gdk-pixbuf-query-loaders \
             "$S/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders"; do
        if [ -x "$c" ] || command -v "$c" >/dev/null 2>&1; then QL="$c"; break; fi
    done
    if [ -n "$QL" ]; then
        GDK_PIXBUF_MODULEDIR="$LOADERS" "$QL" > "$CACHE" 2>/dev/null || true
        [ -f "$CACHE" ] && sed_i "$CACHE"
    fi
fi

# GTK input method modules.
IMDIR="$S/usr/lib/x86_64-linux-gnu/gtk-3.0/3.0.0/immodules"
IMCACHE="$S/usr/lib/x86_64-linux-gnu/gtk-3.0/3.0.0/immodules.cache"
if [ -d "$IMDIR" ] && command -v gtk-query-immodules-3.0 >/dev/null 2>&1; then
    gtk-query-immodules-3.0 "$IMDIR"/*.so > "$IMCACHE" 2>/dev/null || true
    [ -f "$IMCACHE" ] && sed_i "$IMCACHE"
fi

# Shared MIME database.
if [ -d "$S/usr/share/mime" ]; then
    update-mime-database "$S/usr/share/mime" >/dev/null 2>&1 || true
fi

# Icon theme caches (best effort).
for d in "$S"/usr/share/icons/*; do
    if [ -f "$d/index.theme" ] && command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f "$d" >/dev/null 2>&1 || true
    fi
done

# Dynamic loader cache. The runtime mounts the sysroot at /, so write the
# cache with root-relative paths (ldconfig -r strips the stage prefix).
CONFD="$S/etc/ld.so.conf.d"
mkdir -p "$CONFD"
printf '%s\n' /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu > "$CONFD/kyronix.conf"
printf '%s\n' 'include /etc/ld.so.conf.d/*.conf' > "$S/etc/ld.so.conf"
if command -v ldconfig >/dev/null 2>&1; then
    ldconfig -r "$S" -c new >/dev/null 2>&1 || true
fi

echo CACHE_OK
