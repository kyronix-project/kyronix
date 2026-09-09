# Install the pre-extracted GTK3 stack (from the top-level gtk/ directory)
# into the Kyronix rootfs.
#
# gtk/ is the unpacked tree of the GTK3 runtime from Alpine 3.20 (all deps
# included: glib 2.82, pango, cairo, gdk-pixbuf, atk, X libraries, icon
# themes, etc). We merge usr/lib into /usr/lib/wayland so the dynamic
# loader (which resolves /usr/lib/wayland first) picks up the newer
# GTK-provided copies of glib/pango over the older wayland-blob ones.

GTK     := gtk3
ROOTFS  := ../../../../rootfs
BUILD   := ../../../../build/gtk-cache

ROOTFS_ABS := $(abspath $(ROOTFS))
BUILD_ABS  := $(abspath $(BUILD))

.PHONY: all install cache clean

all: install cache

install:
	@mkdir -p $(ROOTFS)/usr/lib/wayland
	@cp -a $(GTK)/usr/lib/. $(ROOTFS)/usr/lib/wayland/
	@mkdir -p $(ROOTFS)/usr/bin $(ROOTFS)/usr/libexec
	@cp -a $(GTK)/usr/bin/* $(ROOTFS)/usr/bin/ 2>/dev/null || true
	@cp -a $(GTK)/usr/libexec/* $(ROOTFS)/usr/libexec/ 2>/dev/null || true
	@cp -a $(GTK)/usr/share/. $(ROOTFS)/usr/share/ 2>/dev/null || true
	@mkdir -p $(ROOTFS)/etc
	@cp -a $(GTK)/etc/. $(ROOTFS)/etc/ 2>/dev/null || true
	@sh prune.sh $(ROOTFS)
	@echo "  gtk: GTK3 stack installed into rootfs"

# Regenerate the GTK runtime caches (module lists + compiled GSettings
# schemas) and place them back into the rootfs. Requires the Alpine musl
# binaries from gtk/usr/bin, so these must run inside the build container
# (they pull the modules' transitive deps from the rootfs loader path).
# cache depends on install: it reads rootfs/usr/lib/wayland which install
# writes, so the two must never run in parallel under make -j.
cache: install
	@mkdir -p $(BUILD_ABS)/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders \
	         $(BUILD_ABS)/usr/lib/gtk-3.0/3.0.0/immodules $(BUILD_ABS)/schemas
	@for f in $(ROOTFS_ABS)/usr/lib/wayland/gdk-pixbuf-2.0/2.10.0/loaders/*.so; do \
	    ln -sf "$$(readlink -f "$$f")" $(BUILD_ABS)/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/$$(basename "$$f"); done
	@for f in $(ROOTFS_ABS)/usr/lib/wayland/gtk-3.0/3.0.0/immodules/*.so; do \
	    ln -sf "$$(readlink -f "$$f")" $(BUILD_ABS)/usr/lib/gtk-3.0/3.0.0/immodules/$$(basename "$$f"); done
	@export LD_LIBRARY_PATH=$(ROOTFS_ABS)/usr/lib/wayland:$(ROOTFS_ABS)/usr/lib; \
	 $(GTK)/usr/bin/gdk-pixbuf-query-loaders \
	    $(BUILD_ABS)/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/*.so > $(BUILD_ABS)/loaders.cache
	@export LD_LIBRARY_PATH=$(ROOTFS_ABS)/usr/lib/wayland:$(ROOTFS_ABS)/usr/lib; \
	 $(GTK)/usr/bin/gtk-query-immodules-3.0 \
	    $(BUILD_ABS)/usr/lib/gtk-3.0/3.0.0/immodules/*.so > $(BUILD_ABS)/immodules.cache
	@$(GTK)/usr/bin/glib-compile-schemas --targetdir=$(BUILD_ABS)/schemas \
	    $(GTK)/usr/share/glib-2.0/schemas
	@sed -i 's|$(BUILD_ABS)/usr/lib|/usr/lib/wayland|' $(BUILD_ABS)/loaders.cache $(BUILD_ABS)/immodules.cache
	@mkdir -p $(ROOTFS)/usr/lib/gdk-pixbuf-2.0/2.10.0 \
	         $(ROOTFS)/usr/lib/gtk-3.0/3.0.0 \
	         $(ROOTFS)/usr/share/glib-2.0/schemas
	@cp $(BUILD_ABS)/loaders.cache $(ROOTFS)/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache
	@cp $(BUILD_ABS)/immodules.cache $(ROOTFS)/usr/lib/gtk-3.0/3.0.0/immodules.cache
	@cp $(BUILD_ABS)/schemas/gschemas.compiled $(ROOTFS)/usr/share/glib-2.0/schemas/gschemas.compiled
	@echo "  gtk: GTK3 caches generated (loaders/immodules/schemas)"

clean:
	@echo "  gtk: clean (source tree kept at $(GTK))"
