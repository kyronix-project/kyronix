BOOT_FAT        := $(BUILD)/installer-boot.fat
INITRD_ROOT     := $(BUILD)/initrd-root
ISO_ROOT        := $(BUILD)/iso-root
WESTON_INITRD   := $(BUILD)/initrd-weston.cpio
CONSOLE_INITRD  := $(BUILD)/initrd-console.cpio

ROOTFS_SOURCES := $(shell find rootfs -type f \
	-not -path 'rootfs/bin/*' \
	-not -path 'rootfs/usr/libexec/*' \
	-not -path 'rootfs/usr/lib/wayland/*' \
	-not -path 'rootfs/usr/lib/weston/*' \
	-not -path 'rootfs/usr/lib/libweston-9/*' \
	-not -path 'rootfs/usr/bin/weston*' \
	-not -path 'rootfs/usr/share/X11/*' \
	-not -path 'rootfs/usr/share/weston/*' \
	-not -path 'rootfs/usr/share/fonts/*' \
	-not -path 'rootfs/etc/fonts/*' \
	-not -name '.gitignore' \
	-not -name 'libacl.so*' \
	-not -name 'libattr.so*' | sort)

LIMINE_FILES := \
	$(LIMINE)/limine-bios.sys \
	$(LIMINE)/limine-bios-cd.bin \
	$(LIMINE)/limine-uefi-cd.bin \
	$(LIMINE)/BOOTX64.EFI \
	$(LIMINE)/BOOTIA32.EFI

$(LIMINE)/limine: $(LIMINE)/limine.c
	$(HOSTCC) $< -o $@

$(BOOT_FAT): $(KERNEL) boot/limine-disk.conf boot/wallpaper.jpg $(LIMINE)/limine-bios.sys
	@mkdir -p $(@D)
	rm -f $@
	truncate -s 16M $@
	mkfs.vfat -F 16 -n KYRONIXBOOT $@ >/dev/null
	mmd -i $@ ::/boot ::/boot/limine ::/limine
	mcopy -i $@ $(KERNEL) ::/boot/kernel.elf
	mcopy -i $@ boot/limine-disk.conf ::/boot/limine/limine.conf
	mcopy -i $@ boot/wallpaper.jpg ::/limine/wallpaper.jpg
	mcopy -i $@ $(LIMINE)/limine-bios.sys ::/boot/limine/
	@echo "  Built: $@"

# --- shared initrd root filesystem ---
$(INITRD_ROOT): $(KERNEL) $(KERNEL_MODULES) $(BOOT_FAT) $(USERSPACE_STAMP) $(ROOTFS_SOURCES)
	rm -rf $(INITRD_ROOT)
	mkdir -p $(INITRD_ROOT)/boot/limine
	mkdir -p $(INITRD_ROOT)/lib/modules
	mkdir -p $(INITRD_ROOT)/usr/share/kyronix
	cp -a rootfs/. $(INITRD_ROOT)/
	cp $(KERNEL_MODULES) $(INITRD_ROOT)/lib/modules/
	cp $(KERNEL) $(INITRD_ROOT)/boot/kernel.elf
	cp boot/limine-disk.conf $(INITRD_ROOT)/boot/limine/limine.conf
	cp boot/wallpaper.jpg $(INITRD_ROOT)/boot/limine/wallpaper.jpg
	cp $(LIMINE)/limine-bios.sys $(INITRD_ROOT)/boot/limine/
	split -b 1M -d -a 2 $(BOOT_FAT) \
	    $(INITRD_ROOT)/usr/share/kyronix/boot.fat.

# --- weston variant (desktop) ---
$(WESTON_INITRD): $(INITRD_ROOT) rootfs/etc/rc.conf.weston
	cp rootfs/etc/rc.conf.weston $(INITRD_ROOT)/etc/rc.conf
	@cd $(INITRD_ROOT) && find . -not -name '.gitignore' | sort | \
	    cpio -o --format=newc --owner=0:0 --reproducible \
	    > "$(abspath $(WESTON_INITRD))" 2>/dev/null
	@echo "  Built: $(WESTON_INITRD)"

# --- console variant (login shell) ---
# depends on WESTON_INITRD to serialize shared INITRD_ROOT usage
$(CONSOLE_INITRD): $(INITRD_ROOT) $(WESTON_INITRD) rootfs/etc/rc.conf.console
	cp rootfs/etc/rc.conf.console $(INITRD_ROOT)/etc/rc.conf
	@cd $(INITRD_ROOT) && find . -not -name '.gitignore' | sort | \
	    cpio -o --format=newc --owner=0:0 --reproducible \
	    > "$(abspath $(CONSOLE_INITRD))" 2>/dev/null
	@echo "  Built: $(CONSOLE_INITRD)"

define build_iso
	rm -rf $(1)
	mkdir -p $(1)/boot/limine $(1)/EFI/BOOT
	cp $(KERNEL) $(1)/boot/kernel.elf
	cp $(3) $(1)/boot/initrd-weston.cpio
	cp $(4) $(1)/boot/initrd-console.cpio
	cp $(2) $(1)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(1)/boot/limine/
	cp $(LIMINE)/limine-bios-cd.bin $(1)/boot/limine/
	cp $(LIMINE)/limine-uefi-cd.bin $(1)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(1)/EFI/BOOT/
	cp $(LIMINE)/BOOTIA32.EFI $(1)/EFI/BOOT/
	if [ -n "$(5)" ]; then \
	    mkdir -p $(1)/limine; \
	    cp $(5) $(1)/limine/wallpaper.jpg; \
	fi
	xorriso -as mkisofs \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(1) -o $(6)
	./$(LIMINE)/limine bios-install $(6)
	@echo "  Built: $(6)"
endef

$(ISO): $(KERNEL) $(WESTON_INITRD) $(CONSOLE_INITRD) boot/limine.conf \
		boot/wallpaper.jpg $(LIMINE_FILES) $(LIMINE)/limine
	@mkdir -p $(@D)
	$(call build_iso,$(ISO_ROOT),boot/limine.conf,$(WESTON_INITRD),$(CONSOLE_INITRD),boot/wallpaper.jpg,$(ISO))

$(DISK):
	@mkdir -p $(@D)
	truncate -s 512M $@
	@echo "  Created persistent install disk: $@"
