BOOT_FAT    := $(BUILD)/installer-boot.fat
INITRD_ROOT := $(BUILD)/initrd-root
ISO_ROOT    := $(BUILD)/iso-root

ROOTFS_SOURCES := $(shell find rootfs -type f \
	-not -path 'rootfs/bin/*' \
	-not -path 'rootfs/usr/libexec/*' \
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

$(BOOT_FAT): $(KERNEL) limine-disk.conf $(LIMINE)/limine-bios.sys
	@mkdir -p $(@D)
	rm -f $@
	truncate -s 16M $@
	mkfs.vfat -F 16 -n KYRONIXBOOT $@ >/dev/null
	mmd -i $@ ::/boot ::/boot/limine
	mcopy -i $@ $(KERNEL) ::/boot/kernel.elf
	mcopy -i $@ limine-disk.conf ::/boot/limine/limine.conf
	mcopy -i $@ $(LIMINE)/limine-bios.sys ::/boot/limine/
	@echo "  Built: $@"

$(INITRD): $(KERNEL) $(KERNEL_MODULES) $(BOOT_FAT) $(USERSPACE_STAMP) $(ROOTFS_SOURCES) \
		limine-disk.conf $(LIMINE)/limine-bios.sys
	rm -rf $(INITRD_ROOT)
	mkdir -p $(INITRD_ROOT)/boot/limine
	mkdir -p $(INITRD_ROOT)/lib/modules
	mkdir -p $(INITRD_ROOT)/usr/share/kyronix
	cp -a rootfs/. $(INITRD_ROOT)/
	cp $(KERNEL_MODULES) $(INITRD_ROOT)/lib/modules/
	cp $(KERNEL) $(INITRD_ROOT)/boot/kernel.elf
	cp limine-disk.conf $(INITRD_ROOT)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(INITRD_ROOT)/boot/limine/
	split -b 1M -d -a 2 $(BOOT_FAT) \
	    $(INITRD_ROOT)/usr/share/kyronix/boot.fat.
	@cd $(INITRD_ROOT) && find . -not -name '.gitignore' | sort | \
	    cpio -o --format=newc --owner=0:0 --reproducible \
	    > $(abspath $(INITRD)) 2>/dev/null
	@echo "  Built: $(INITRD)"

define build_iso
	rm -rf $(1)
	mkdir -p $(1)/boot/limine $(1)/EFI/BOOT
	cp $(KERNEL) $(1)/boot/kernel.elf
	cp $(3) $(1)/boot/initrd.cpio
	cp $(2) $(1)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys $(1)/boot/limine/
	cp $(LIMINE)/limine-bios-cd.bin $(1)/boot/limine/
	cp $(LIMINE)/limine-uefi-cd.bin $(1)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI $(1)/EFI/BOOT/
	cp $(LIMINE)/BOOTIA32.EFI $(1)/EFI/BOOT/
	xorriso -as mkisofs \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    $(1) -o $(4)
	./$(LIMINE)/limine bios-install $(4)
	@echo "  Built: $(4)"
endef

$(ISO): $(KERNEL) $(INITRD) limine.conf $(LIMINE_FILES) $(LIMINE)/limine
	@mkdir -p $(@D)
	$(call build_iso,$(ISO_ROOT),limine.conf,$(INITRD),$(ISO))

$(DISK):
	@mkdir -p $(@D)
	truncate -s 512M $@
	@echo "  Created persistent install disk: $@"
