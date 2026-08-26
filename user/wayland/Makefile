ALPINE  := https://dl-cdn.alpinelinux.org/alpine
TMPDIR  := /tmp/kyronix-wayland
STAGE   := $(TMPDIR)/stage
ROOTFS  := ../../rootfs

WESTON_VER  := 9.0.0-r0
WESTON_PKGS := \
	weston-$(WESTON_VER) \
	libweston-$(WESTON_VER) \
	libweston-desktop-$(WESTON_VER) \
	weston-backend-fbdev-$(WESTON_VER) \
	weston-backend-headless-$(WESTON_VER) \
	weston-shell-desktop-$(WESTON_VER) \
	weston-terminal-$(WESTON_VER) \
	weston-clients-$(WESTON_VER)

DEP_313 := \
	wayland-libs-server-1.18.0-r5 \
	wayland-libs-client-1.18.0-r5 \
	wayland-libs-cursor-1.18.0-r5 \
	libffi-3.3-r2

DEP_MAIN := \
	libxkbcommon-1.7.0-r0 \
	xkeyboard-config-2.41-r0 \
	pixman-0.43.2-r0 \
	libdrm-2.4.120-r0 \
	dbus-libs-1.14.10-r1 \
	freetype-2.13.2-r0 \
	fontconfig-2.15.0-r1 \
	harfbuzz-8.5.0-r0 \
	graphite2-1.3.14-r6 \
	fribidi-1.0.15-r0 \
	pango-1.52.2-r0 \
	glib-2.80.5-r0 \
	libintl-0.22.5-r0 \
	pcre2-10.43-r0 \
	libwebp-1.3.2-r0 \
	libsharpyuv-1.3.2-r0 \
	libmount-2.40.1-r1 \
	libblkid-2.40.1-r1 \
	libx11-1.8.9-r1 \
	libxcb-1.16.1-r0 \
	libxau-1.0.11-r4 \
	libxdmcp-1.1.5-r1 \
	libxext-1.3.6-r2 \
	libxrender-0.9.11-r5 \
	libcap2-2.78-r0 \
	libeconf-0.6.3-r0 \
	font-dejavu-2.37-r5

DEP_COMMUNITY := \
	libinput-libs-1.25.0-r0 \
	libevdev-1.13.1-r0 \
	mtdev-1.1.6-r3 \
	libelogind-252.9-r5

WESTON_BINS   := weston weston-terminal weston-simple-shm weston-flower weston-image
WESTON_LIBEXEC := weston-desktop-shell weston-keyboard
WESTON_SHELLS := desktop-shell.so kiosk-shell.so

.PHONY: all clean install fetch

all: fetch

fetch: $(TMPDIR)/.extracted

$(TMPDIR)/.fetched: Makefile
	@mkdir -p $(TMPDIR)
	@for p in $(WESTON_PKGS); do \
	    test -s $(TMPDIR)/$$p.apk || \
	    curl -fsSL -o $(TMPDIR)/$$p.apk $(ALPINE)/v3.13/community/x86_64/$$p.apk || \
	    { echo "wayland: failed to fetch $$p"; exit 1; }; \
	done
	@for p in $(DEP_313); do \
	    test -s $(TMPDIR)/$$p.apk || \
	    curl -fsSL -o $(TMPDIR)/$$p.apk $(ALPINE)/v3.13/main/x86_64/$$p.apk || \
	    { echo "wayland: failed to fetch $$p"; exit 1; }; \
	done
	@for p in $(DEP_MAIN); do \
	    test -s $(TMPDIR)/$$p.apk || \
	    curl -fsSL -o $(TMPDIR)/$$p.apk $(ALPINE)/v3.20/main/x86_64/$$p.apk || \
	    { echo "wayland: failed to fetch $$p"; exit 1; }; \
	done
	@for p in $(DEP_COMMUNITY); do \
	    test -s $(TMPDIR)/$$p.apk || \
	    curl -fsSL -o $(TMPDIR)/$$p.apk $(ALPINE)/v3.20/community/x86_64/$$p.apk || \
	    { echo "wayland: failed to fetch $$p"; exit 1; }; \
	done
	@touch $@
	@echo "  wayland: fetched $(words $(WESTON_PKGS) $(DEP_313) $(DEP_MAIN) $(DEP_COMMUNITY)) packages"

$(TMPDIR)/.extracted: $(TMPDIR)/.fetched Makefile
	@rm -rf $(STAGE)
	@mkdir -p $(STAGE)
	@for p in $(WESTON_PKGS) $(DEP_313) $(DEP_MAIN) $(DEP_COMMUNITY); do \
	    tar -xzf $(TMPDIR)/$$p.apk -C $(STAGE) 2>/dev/null; \
	done
	@touch $@
	@echo "  wayland: extracted to $(STAGE)"

install: fetch
	@mkdir -p $(ROOTFS)/usr/lib/wayland $(ROOTFS)/usr/lib/weston \
	    $(ROOTFS)/usr/lib/libweston-9 $(ROOTFS)/usr/libexec \
	    $(ROOTFS)/usr/share/fonts $(ROOTFS)/usr/bin
	@for so in $(STAGE)/usr/lib/*.so* $(STAGE)/lib/*.so*; do \
	    case $$(basename $$so) in \
	        libpangoxft*|libdrm_*) continue ;; \
	    esac; \
	    cp -a $$so $(ROOTFS)/usr/lib/wayland/; \
	done
	@for shell in $(WESTON_SHELLS) libexec_weston.so libexec_weston.so.0 \
	    libexec_weston.so.0.0.0; do \
	    cp -a $(STAGE)/usr/lib/weston/$$shell $(ROOTFS)/usr/lib/weston/; \
	done
	@cp -a $(STAGE)/usr/lib/libweston-9/fbdev-backend.so $(ROOTFS)/usr/lib/libweston-9/
	@cp -a $(STAGE)/usr/lib/libweston-9/headless-backend.so $(ROOTFS)/usr/lib/libweston-9/
	@for b in $(WESTON_BINS); do cp -a $(STAGE)/usr/bin/$$b $(ROOTFS)/usr/bin/; done
	@for b in $(WESTON_LIBEXEC); do cp -a $(STAGE)/usr/libexec/$$b $(ROOTFS)/usr/libexec/; done
	@mkdir -p $(ROOTFS)/usr/share/X11
	@cp -a $(STAGE)/usr/share/X11/xkb $(ROOTFS)/usr/share/X11/
	@cp -a $(STAGE)/usr/share/weston $(ROOTFS)/usr/share/
	@cp -a $(STAGE)/usr/share/fonts/dejavu/DejaVuSansMono.ttf $(ROOTFS)/usr/share/fonts/
	@cp -a $(STAGE)/usr/share/fonts/dejavu/DejaVuSans.ttf $(ROOTFS)/usr/share/fonts/
	@mkdir -p $(ROOTFS)/etc/fonts
	@cp -a $(STAGE)/etc/fonts/. $(ROOTFS)/etc/fonts/
	@echo "  wayland: Weston $(WESTON_VER) installed into rootfs"

clean:
	rm -rf $(ROOTFS)/usr/lib/wayland $(ROOTFS)/usr/lib/weston \
	    $(ROOTFS)/usr/lib/libweston-9 $(ROOTFS)/usr/share/X11 \
	    $(ROOTFS)/usr/share/weston $(ROOTFS)/usr/share/fonts $(ROOTFS)/etc/fonts
	rm -f $(addprefix $(ROOTFS)/usr/bin/,$(WESTON_BINS))
	rm -f $(addprefix $(ROOTFS)/usr/libexec/,$(WESTON_LIBEXEC))
	rm -f $(TMPDIR)/.extracted
