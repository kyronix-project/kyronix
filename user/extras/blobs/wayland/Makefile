STAGE  := stage
ROOTFS := ../../../../rootfs

WESTON_BINS    := weston weston-terminal weston-simple-shm weston-flower weston-image
WESTON_LIBEXEC := weston-desktop-shell weston-keyboard
WESTON_SHELLS  := desktop-shell.so kiosk-shell.so

ICON_DIR := $(ROOTFS)/usr/share/icons
CURSORS  := $(ICON_DIR)/whiteglass/cursors

.PHONY: all clean install

all: install

install: install-weston install-cursors

install-weston:
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
	@echo "  wayland: Weston installed"

install-cursors:
	@mkdir -p $(CURSORS)
	@tar -xzf xcursor-themes-1.0.7-r0.apk -C $(STAGE) 2>/dev/null
	@cp -a $(STAGE)/usr/share/icons/whiteglass/cursors/* $(CURSORS)/
	@mkdir -p $(ICON_DIR)/whiteglass
	@test -f $(STAGE)/usr/share/icons/whiteglass/index.theme && \
		cp -a $(STAGE)/usr/share/icons/whiteglass/index.theme $(ICON_DIR)/whiteglass/ || \
		printf '[Icon Theme]\nName=whiteglass\nComment=White glass cursor theme\nDirectories=cursors\n[cursors]\nSize=32\nType=Scalable\n' > $(ICON_DIR)/whiteglass/index.theme
	@cd $(CURSORS) && ln -sf left_ptr default
	@cd $(CURSORS) && ln -sf left_ptr left-arrow
	@echo "  wayland: whiteglass cursor theme installed"

clean:
	rm -rf $(ROOTFS)/usr/lib/wayland $(ROOTFS)/usr/lib/weston \
	    $(ROOTFS)/usr/lib/libweston-9 $(ROOTFS)/usr/share/X11 \
	    $(ROOTFS)/usr/share/weston $(ROOTFS)/usr/share/fonts $(ROOTFS)/etc/fonts
	rm -f $(addprefix $(ROOTFS)/usr/bin/,$(WESTON_BINS))
	rm -f $(addprefix $(ROOTFS)/usr/libexec/,$(WESTON_LIBEXEC))
	rm -rf $(ROOTFS)/usr/share/icons/whiteglass
