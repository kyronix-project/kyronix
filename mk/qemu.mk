QEMU       ?= qemu-system-x86_64
QEMU_ACCEL ?= $(if $(wildcard /dev/kvm),kvm,tcg)
QEMU_CPU   ?= $(if $(filter kvm,$(QEMU_ACCEL)),host,max)

ifeq ($(QEMU_ACCEL),kvm)
QEMU_ACCEL_ARGS := -enable-kvm -cpu $(QEMU_CPU)
else
QEMU_ACCEL_ARGS := -cpu $(QEMU_CPU)
endif

QEMU_DISK_ARGS = \
	-drive file=$(DISK),format=raw,if=none,id=hd0,cache=writethrough \
	-device ahci,id=ahci \
	-device ide-hd,drive=hd0,bus=ahci.0

.PHONY: run boot

run: $(ISO) $(DISK)
	@$(call phase,Launching QEMU (live ISO))
	$(QEMU) \
	    -M q35 $(QEMU_ACCEL_ARGS) -smp 4 -m 2G \
	    -cdrom $(ISO) -boot d \
	    -serial stdio \
	    -vga qxl -global qxl-vga.vgamem_mb=1024 \
	    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
	    $(QEMU_DISK_ARGS)

boot: $(DISK)
	@$(call phase,Launching QEMU (installed disk))
	$(QEMU) \
	    -M q35 $(QEMU_ACCEL_ARGS) -smp 4 -m 2G \
	    -boot c -serial stdio \
	    -vga qxl -global qxl-vga.vgamem_mb=1024 \
	    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
	    $(QEMU_DISK_ARGS)
