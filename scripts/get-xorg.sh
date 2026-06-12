#!/bin/bash
# Downloads Alpine 3.21 musl Xorg packages into rootfs/
set -e
ROOTFS="$(cd "$(dirname "$0")/.." && pwd)/rootfs"
CACHE=/tmp/xorg-pkgs
MAIN="https://dl-cdn.alpinelinux.org/alpine/v3.21/main/x86_64"
COMM="https://dl-cdn.alpinelinux.org/alpine/v3.21/community/x86_64"

MAIN_PKGS=(
    "pixman-0.43.4-r1"
    "libxau-1.0.11-r4"
    "libxdmcp-1.1.5-r1"
    "libxcb-1.16.1-r0"
    "libx11-1.8.10-r0"
    "libxext-1.3.6-r2"
    "libxrender-0.9.11-r5"
    "libxft-2.3.8-r3"
    "libice-1.1.1-r6"
    "libsm-1.2.4-r4"
    "libxt-1.3.1-r0"
    "libxmu-1.2.1-r0"
    "libxpm-3.5.19-r0"
    "libxaw-1.0.16-r1"
    "libxkbfile-1.1.3-r0"
    "libxshmfence-1.3.2-r6"
    "libpciaccess-0.18.1-r0"
    "libuuid-2.40.4-r1"
    "nettle-3.10.2-r0"
    "libfontenc-1.1.8-r0"
    "freetype-2.13.3-r0"
    "fontconfig-2.15.0-r1"
    "libexpat-2.7.5-r0"
    "expat-2.7.5-r0"
    "zlib-1.3.2-r0"
    "libncursesw-6.5_p20241006-r3"
    "ncurses-terminfo-base-6.5_p20241006-r3"
    "libdrm-2.4.123-r1"
    "bzip2-1.0.8-r6"
    "libpng-1.6.57-r0"
    "brotli-libs-1.1.0-r2"
    "libbz2-1.0.8-r6"
    "gmp-6.3.0-r2"
    "libmd-1.1.0-r0"
    "libbsd-0.12.2-r0"
    "xkbcomp-1.5.0-r0"
    "xkeyboard-config-2.43-r0"
    "font-misc-misc-1.1.3-r1"
    "font-cursor-misc-1.0.4-r1"
    "mkfontscale-1.2.3-r1"
    "libxcursor-1.2.3-r0"
    "libxfixes-6.0.1-r4"
    "libxi-1.8.2-r0"
)

COMM_PKGS=(
    "xorg-server-21.1.16-r0"
    "xorg-server-common-21.1.16-r0"
    "xf86-video-fbdev-0.5.0-r6"
    "xf86-input-evdev-2.10.6-r2"
    "libxcvt-0.1.2-r0"
    "libxfont2-2.0.7-r0"
    "libevdev-1.13.3-r0"
    "mtdev-1.1.7-r0"
    "twm-1.0.12-r3"
    "xterm-396-r0"
    "xclock-1.1.1-r0"
    "xsetroot-1.1.3-r1"
    "xcalc-1.1.2-r0"
    "xeyes-1.3.0-r0"
)

mkdir -p "$CACHE"

fetch_pkg() {
    local url="$1" pkg="$2"
    local dest="$CACHE/${pkg}.apk"
    if [ ! -f "$dest" ]; then
        echo "  GET $pkg"
        curl -fsSL --max-time 60 --retry 3 "$url/${pkg}.apk" -o "$dest"
    else
        echo "  cached $pkg"
    fi
}

extract_pkg() {
    local pkg="$1"
    tar -xzf "$CACHE/${pkg}.apk" -C "$ROOTFS" \
        --exclude='.PKGINFO' --exclude='.SIGN.*' --exclude='.CHANGELOG' \
        --exclude='usr/include' \
        --exclude='usr/share/doc' --exclude='usr/share/man' \
        --exclude='usr/lib/pkgconfig' --exclude='usr/lib/cmake' \
        --exclude='etc/init.d' --exclude='etc/conf.d' \
        2>/dev/null || true
}

echo "=== Fetching Alpine main packages ==="
for p in "${MAIN_PKGS[@]}"; do fetch_pkg "$MAIN" "$p"; done

echo "=== Fetching Alpine community packages ==="
for p in "${COMM_PKGS[@]}"; do fetch_pkg "$COMM" "$p"; done

echo "=== Extracting to rootfs ==="
for p in "${MAIN_PKGS[@]}"; do extract_pkg "$p"; done
for p in "${COMM_PKGS[@]}"; do extract_pkg "$p"; done

echo "=== Building libudev stub ==="
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
gcc -shared -fPIC -O2 -nostdlib \
    -nostdinc -isystem /usr/lib/musl/include \
    -Wl,-soname,libudev.so.1 \
    -o "$ROOTFS/usr/lib/libudev.so.1" \
    "$SCRIPTDIR/libudev_stub.c"
echo "  built libudev.so.1"

echo "=== Fixing symlinks ==="
# musl dynamic linker needs /lib -> /usr/lib or both searched
# create /usr/lib64 -> /usr/lib for any rpath that uses lib64
ln -sf usr/lib "$ROOTFS/lib64" 2>/dev/null || true
# Xorg looks for modules at /usr/lib/xorg/modules
mkdir -p "$ROOTFS/usr/lib/xorg/modules/drivers"
mkdir -p "$ROOTFS/usr/lib/xorg/modules/input"
mkdir -p "$ROOTFS/usr/share/X11/xkb"
mkdir -p "$ROOTFS/tmp"
mkdir -p "$ROOTFS/var/log"
mkdir -p "$ROOTFS/var/lib/xkb"
mkdir -p "$ROOTFS/run"
cat > "$ROOTFS/etc/ld-musl-x86_64.path" <<'EOF'
/lib
/usr/lib
EOF

# mkfontscale rebuilds fonts.dir - run it on the misc fonts
if [ -d "$ROOTFS/usr/share/fonts/misc" ] && command -v mkfontscale >/dev/null 2>&1; then
    mkfontscale "$ROOTFS/usr/share/fonts/misc" 2>/dev/null || true
fi

echo "=== Done. Xorg rootfs populated. ==="
du -sh "$ROOTFS/usr/lib"/*.so* "$ROOTFS/usr/lib/xorg" 2>/dev/null | tail -3
echo "Total usr/lib:"
du -sh "$ROOTFS/usr/lib" 2>/dev/null
