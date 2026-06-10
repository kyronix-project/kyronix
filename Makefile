# KyronixOS build system

TARGET     := kernel.elf
ISO        := kyronix.iso
LIMINE_DIR := limine-binary
BUILD_DIR  := build

ifneq (, $(shell which x86_64-elf-gcc 2>/dev/null))
    CC := x86_64-elf-gcc
    LD := x86_64-elf-ld
else
    CC := gcc
    LD := ld
endif

CFLAGS := \
    -std=c11           \
    -O2                \
    -Wall -Wextra      \
    -Wno-unused-parameter \
    -ffreestanding     \
    -fno-stack-protector \
    -fno-pic -fno-pie  \
    -m64 -march=x86-64 \
    -mno-80387         \
    -mno-mmx           \
    -mno-sse           \
    -mno-sse2          \
    -mno-red-zone      \
    -mcmodel=kernel    \
    -Ikernel           \
    -Ikernel/boot

LDFLAGS := \
    -T linker.ld       \
    -nostdlib          \
    -static            \
    -z max-page-size=0x1000

SRCS := \
    kernel/kernel.c                    \
    kernel/arch/x86_64/gdt.c          \
    kernel/arch/x86_64/idt.c          \
    kernel/arch/x86_64/pit.c          \
    kernel/mm/pmm.c                   \
    kernel/mm/vmm.c                   \
    kernel/mm/heap.c                  \
    kernel/arch/x86_64/syscall_setup.c \
    kernel/syscall/syscall.c          \
    kernel/exec/elf.c                  \
    kernel/exec/process.c              \
    kernel/proc/proc.c                 \
    kernel/proc/signal.c               \
    kernel/fs/vfs.c                    \
    kernel/fs/pipe.c                   \
    kernel/fs/cpio.c                   \
    kernel/drivers/serial.c           \
    kernel/drivers/kbd.c              \
    kernel/drivers/tty.c              \
    kernel/drivers/fb.c               \
    kernel/lib/string.c               \
    kernel/lib/printf.c                \
    kernel/lib/log.c

ASM_SRCS := \
    kernel/arch/x86_64/idt_stubs.S    \
    kernel/arch/x86_64/syscall_entry.S \
    kernel/proc/sched.S               \
    kernel/drivers/psf_font.S

OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o) $(ASM_SRCS:%.S=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

SRC_DIR  := .
INITRD   := initrd.cpio

.PHONY: all iso run run-serial run-uefi clean user-build

all: $(TARGET) $(INITRD)

user-build:
	$(MAKE) -C user

INITRD_DEPS := $(shell find rootfs -not -name '.gitignore' -type f | sort) \
               $(wildcard build/bin/*)

$(INITRD): $(INITRD_DEPS) | user-build
	@cd rootfs && find . -not -name '.gitignore' | sort | cpio -o --format=newc > ../$@ 2>/dev/null
	@echo "  Built: $@"

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) -m64 -march=x86-64 -c $< -o $@

-include $(DEPS)

$(LIMINE_DIR)/limine: $(LIMINE_DIR)/limine.c
	$(MAKE) -C $(LIMINE_DIR)

iso: $(TARGET) $(INITRD) $(LIMINE_DIR)/limine
	rm -rf iso_root
	mkdir -p iso_root/boot/limine
	mkdir -p iso_root/EFI/BOOT

	cp $(TARGET)              iso_root/boot/kernel.elf
	cp $(INITRD)              iso_root/boot/initrd.cpio
	cp limine.conf            iso_root/boot/limine/limine.conf
	cp $(LIMINE_DIR)/limine-bios.sys    iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-bios-cd.bin iso_root/boot/limine/
	cp $(LIMINE_DIR)/limine-uefi-cd.bin iso_root/boot/limine/
	cp $(LIMINE_DIR)/BOOTX64.EFI        iso_root/EFI/BOOT/
	cp $(LIMINE_DIR)/BOOTIA32.EFI       iso_root/EFI/BOOT/

	xorriso -as mkisofs              \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot                \
	    -boot-load-size 4            \
	    -boot-info-table             \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part               \
	    --efi-boot-image             \
	    --protective-msdos-label     \
	    iso_root -o $(ISO)

	./$(LIMINE_DIR)/limine bios-install $(ISO)
	@echo ""
	@echo "  Built: $(ISO)"

run: iso
	qemu-system-x86_64              \
	    -M q35                      \
	    -m 2G                       \
	    -cdrom $(ISO)               \
	    -boot d                     \
	    -serial stdio               \
	    -vga qxl                    \
	    -global qxl-vga.vgamem_mb=1024 \
	    -no-reboot                  \
	    -no-shutdown

run-serial: iso
	qemu-system-x86_64              \
	    -M q35                      \
	    -m 2G                       \
	    -cdrom $(ISO)               \
	    -boot d                     \
	    -display none               \
	    -serial stdio               \
	    -no-reboot                  \
	    -no-shutdown

OVMF ?= /usr/share/edk2/x64/OVMF.fd

run-uefi: iso
	qemu-system-x86_64              \
	    -M q35                      \
	    -m 2G                       \
	    -cdrom $(ISO)               \
	    -bios $(OVMF)               \
	    -boot d                     \
	    -serial stdio               \
	    -vga qxl                    \
	    -global qxl-vga.vgamem_mb=1024 \
	    -no-reboot                  \
	    -no-shutdown

fmt:
	@echo "Formatting code..."
	@find $(SRC_DIR) -type f \( -name "*.c" -o -name "*.h" \) -exec clang-format -i {} \;
	@echo "Format complete"

fmt-check:
	@echo "Checking code style..."
	@find $(SRC_DIR) -type f \( -name "*.c" -o -name "*.h" \) -exec clang-format --dry-run -Werror {} +

clean:
	rm -f $(TARGET) $(ISO) $(INITRD)
	rm -rf $(BUILD_DIR) iso_root rootfs/bin
	$(MAKE) -C user clean
	$(MAKE) -C $(LIMINE_DIR) clean 2>/dev/null; true
