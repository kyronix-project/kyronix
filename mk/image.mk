BOOT_FAT        := $(BUILD)/installer-boot.fat
INITRD_ROOT     := $(BUILD)/initrd-root
ISO_ROOT        := $(BUILD)/iso-root
INITRD          := $(BUILD)/initrd.cpio

RELEASE ?= 0
STATUS  := $(if $(filter 1,$(RELEASE)),RELEASE,INDEV)
OS_RELEASE := $(BUILD)/os-release-$(STATUS)
PKG_LIST   := $(wildcard packages.list)

$(OS_RELEASE):
	@mkdir -p $(@D)
	@printf 'NAME="Kyronix"\n' > $@
	@printf 'ID="kyronix"\n' >> $@
	@printf 'VERSION="$(VERSION)"\n' >> $@
	@printf 'VERSION_ID="$(VERSION)"\n' >> $@
	@printf 'PRETTY_NAME="Kyronix $(VERSION)-$(STATUS)"\n' >> $@
	@printf 'HOME_URL="https://github.com/kyronix-project/kyronix"\n' >> $@
	@printf 'BUG_REPORT_URL="https://github.com/kyronix-project/kyronix/issues"\n' >> $@
	@$(call ok,$@ ($(STATUS)))

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

$(LIMINE)/limine: $(LIMINE)/limine.c mk/image.mk
	@$(call step,CC $<)
	@$(HOSTCC) -static $< -o $@

$(BOOT_FAT): $(KERNEL) boot/limine-disk.conf boot/wallpaper.jpg $(LIMINE)/limine-bios.sys
	@$(call phase,Boot FAT)
	@mkdir -p $(@D)
	@rm -f $@
	@truncate -s 16M $@
	@mkfs.vfat -F 16 -n KYRONIXBOOT $@ >/dev/null
	@mmd -i $@ ::/boot ::/boot/limine ::/limine
	@mcopy -i $@ $(KERNEL) ::/boot/kernel.elf
	@mcopy -i $@ boot/limine-disk.conf ::/boot/limine/limine.conf
	@mcopy -i $@ boot/wallpaper.jpg ::/limine/wallpaper.jpg
	@mcopy -i $@ $(LIMINE)/limine-bios.sys ::/boot/limine/
	@$(call ok,$@)

# --- shared initrd root filesystem ---
$(INITRD_ROOT): $(KERNEL) $(KERNEL_MODULES) $(BOOT_FAT) $(USERSPACE_STAMP) $(ROOTFS_SOURCES) $(OS_RELEASE) $(PKG_LIST) $(GALCULATOR_STAMP)
	@if [ -d $(INITRD_ROOT) ]; then chmod -R u+w $(INITRD_ROOT) 2>/dev/null || true; rm -rf $(INITRD_ROOT); fi
	@mkdir -p $(INITRD_ROOT)/boot/limine
	@mkdir -p $(INITRD_ROOT)/lib/modules
	@mkdir -p $(INITRD_ROOT)/usr/share/kyronix
	@cp -a rootfs/. $(INITRD_ROOT)/
	@if [ -f packages.list ]; then \
		$(call phase,Local packages); \
		./scripts/local-pkg-install --registry ./pkg-registry-src --target $(abspath $(INITRD_ROOT)) --list packages.list || true; \
		$(call ok,Local packages); \
	fi
	@mkdir -p $(INITRD_ROOT)/var/lib/flatpak $(INITRD_ROOT)/var/lib/dbus \
	    $(INITRD_ROOT)/run/user/0 $(INITRD_ROOT)/dev/shm
	@chmod 0700 $(INITRD_ROOT)/run/user/0
	@chmod 01777 $(INITRD_ROOT)/dev/shm
	@ln -sf /etc/machine-id $(INITRD_ROOT)/var/lib/dbus/machine-id
	@cp $(OS_RELEASE) $(INITRD_ROOT)/etc/os-release
	@cp $(KERNEL_MODULES) $(INITRD_ROOT)/lib/modules/
	@cp $(KERNEL) $(INITRD_ROOT)/boot/kernel.elf
	@cp boot/limine-disk.conf $(INITRD_ROOT)/boot/limine/limine.conf
	@cp boot/wallpaper.jpg $(INITRD_ROOT)/boot/limine/wallpaper.jpg
	@cp $(LIMINE)/limine-bios.sys $(INITRD_ROOT)/boot/limine/
	@split -b 1M -d -a 2 $(BOOT_FAT) \
	    $(INITRD_ROOT)/usr/share/kyronix/boot.fat.
	@if [ -s $(GALCULATOR_APPIMG) ]; then \
	    mkdir -p $(INITRD_ROOT)/usr/share/appimages; \
	    cp $(GALCULATOR_APPIMG) $(INITRD_ROOT)/usr/share/appimages/galculator.AppImage; \
	fi
	@touch $(INITRD_ROOT)
	@$(call ok,initrd root)

# --- initrd (single image; the boot entry's cmdline picks init and rc.conf) ---
$(INITRD): $(INITRD_ROOT)
	@$(call phase,Initrd)
	@cd $(INITRD_ROOT) && find . -not -name '.gitignore' | sort | \
	    cpio -o --format=newc --owner=0:0 --reproducible \
	    > "$(abspath $(INITRD))" 2>/dev/null
	@$(call ok,$(INITRD))

define build_iso
	@rm -rf $(1)
	@mkdir -p $(1)/boot/limine $(1)/EFI/BOOT
	@cp $(KERNEL) $(1)/boot/kernel.elf
	@cp $(3) $(1)/boot/initrd.cpio
	@cp $(2) $(1)/boot/limine/limine.conf
	@cp $(LIMINE)/limine-bios.sys $(1)/boot/limine/
	@cp $(LIMINE)/limine-bios-cd.bin $(1)/boot/limine/
	@cp $(LIMINE)/limine-uefi-cd.bin $(1)/boot/limine/
	@cp $(LIMINE)/BOOTX64.EFI $(1)/EFI/BOOT/
	@cp $(LIMINE)/BOOTIA32.EFI $(1)/EFI/BOOT/
	@if [ -n "$(4)" ]; then \
	    mkdir -p $(1)/limine; \
	    cp $(4) $(1)/limine/wallpaper.jpg; \
	fi
	@xorriso -as mkisofs \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(1) -o $(5)
	@./$(LIMINE)/limine bios-install $(5)
	@$(call ok,$(5))
endef

$(ISO): $(KERNEL) $(INITRD) boot/limine.conf \
		boot/wallpaper.jpg $(LIMINE_FILES) $(LIMINE)/limine
	@$(call phase,ISO)
	@mkdir -p $(@D)
	$(call build_iso,$(ISO_ROOT),boot/limine.conf,$(INITRD),boot/wallpaper.jpg,$(ISO))

$(DISK):
	@mkdir -p $(@D)
	truncate -s 16G $@
	@$(call ok,Created persistent install disk: $@)
