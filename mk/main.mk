VERSION  := $(shell sed -n 's/ *#define *KERNEL_VERSION *"\(.*\)"/\1/p' kernel/version.h)
ARCH     := amd64
BUILD    := build
DIST     := dist

KERNEL   := $(BUILD)/kernel.elf
ISO      := $(DIST)/kyronix.iso
LIVE_ISO := $(DIST)/kyronix-$(VERSION)-RELEASE-$(ARCH)-live.iso
DISK     := $(DIST)/kyronix-disk.img
TEST_ISO := $(DIST)/kyronix-test.iso

include mk/common.mk
include mk/kernel.mk
include mk/userspace.mk
include mk/image.mk
include mk/qemu.mk
include mk/test.mk

.PHONY: iso live help clean

iso: $(ISO)

live:
	$(MAKE) $(ISO) RELEASE=1
	@mkdir -p $(DIST)
	@rm -f $(LIVE_ISO)
	mv $(ISO) $(LIVE_ISO)
	sha256sum $(LIVE_ISO) > $(LIVE_ISO).sha256
	@echo "  Built:  $(LIVE_ISO)"
	@echo "  Checksum: $(LIVE_ISO).sha256"
	@echo "  Variants: Weston+Console"

help:
	@echo "Kyronix build commands"
	@echo ""
	@echo "  make          Build $(ISO) (Desktop + Console variants)"
	@echo "  make iso      Build $(ISO) (Desktop + Console variants)"
	@echo "  make live     Build a RELEASE image: $(LIVE_ISO) (+ .sha256)"
	@echo "  make run      Build and boot the live ISO with $(DISK)"
	@echo "  make boot     Boot the system already installed on $(DISK)"
	@echo "  make test     Build and run the test suite in QEMU"
	@echo "  make iso INSTRUMENT=1  Build with frame pointers (kmemleak/profiler backtraces)"
	@echo "  make clean    Remove build output (the installed disk is kept)"
	@echo ""
	@echo "Container build: append CRUNTIME=podman or CRUNTIME=docker"

clean:
	rm -rf $(BUILD)
	rm -rf .config include/config include/generated scripts/kconfig
	rm -rf build/iso-root build/test-rootfs build/test-iso-root
	rm -f test.log
	rm -f $(ISO) $(TEST_ISO)
	rm -f $(BUILD)/initrd-weston.cpio $(BUILD)/initrd-console.cpio
	rm -f $(DIST)/kernel.elf $(DIST)/kyronix-boot.fat
	rm -f $(DIST)/test-initrd.cpio $(DIST)/test-disk.img $(DIST)/disk.img
	rm -f $(DIST)/kyronix-*-$(ARCH).iso
	rm -f $(DIST)/kyronix-*-$(ARCH)-live.iso $(DIST)/kyronix-*-$(ARCH)-test.iso
	rm -f $(DIST)/kyronix-*-$(ARCH)-live.iso.sha256
	rm -f $(BUILD)/os-release-INDEV $(BUILD)/os-release-RELEASE
	$(MAKE) -C user clean
	$(MAKE) -C limine clean 2>/dev/null || true
	@rmdir $(DIST) 2>/dev/null || true
	@echo "  Kept: $(DISK)"
