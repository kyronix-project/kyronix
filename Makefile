# KyronixOS build system

TARGET     := kernel.elf
ISO        := kyronix.iso
LIMINE_DIR := limine
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
    kernel/mm/vma.c                   \
    kernel/mm/heap.c                  \
    kernel/mm/shm.c                   \
    kernel/arch/x86_64/syscall_setup.c \
    kernel/syscall/syscall.c          \
    kernel/syscall/epoll.c            \
    kernel/syscall/file.c             \
    kernel/syscall/poll.c             \
    kernel/syscall/socket.c           \
    kernel/syscall/time.c             \
    kernel/exec/elf.c                  \
    kernel/exec/process.c              \
    kernel/proc/proc.c                 \
    kernel/proc/signal.c               \
    kernel/fs/vfs.c                    \
    kernel/fs/devfs.c                  \
    kernel/fs/eventfd.c                \
    kernel/fs/fdctl.c                  \
    kernel/fs/fdpipe.c                 \
    kernel/fs/procfs.c                 \
    kernel/fs/unix_socket.c            \
    kernel/fs/pipe.c                   \
    kernel/fs/cpio.c                   \
    kernel/drivers/serial.c           \
    kernel/drivers/kbd.c              \
    kernel/drivers/tty.c              \
    kernel/drivers/fb.c               \
    kernel/drivers/pci.c              \
    kernel/drivers/uio.c              \
    kernel/drivers/fbdev.c            \
    kernel/drivers/input.c            \
    kernel/drivers/ps2mouse.c         \
    kernel/drivers/vt.c               \
    kernel/lib/string.c               \
    kernel/lib/printf.c                \
    kernel/lib/log.c

ASM_SRCS := \
    kernel/arch/x86_64/idt_stubs.S    \
    kernel/arch/x86_64/syscall_entry.S \
    kernel/proc/sched.S                \
    kernel/drivers/psf_font.S

OBJS := $(SRCS:%.c=$(BUILD_DIR)/%.o) $(ASM_SRCS:%.S=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

SRC_DIR  := .
INITRD   := initrd.cpio

.PHONY: all iso run run-serial run-uefi clean user-build xorg testrunner test-initrd test-iso test-run test-run-log fmt fmt-check

all: $(TARGET) $(INITRD)

build/libatomic_asneeded.a:
	@mkdir -p $(@D)
	ar rcs $@

user-build: build/libatomic_asneeded.a
	$(MAKE) -C user

xorg:
	@bash scripts/get-xorg.sh

INITRD_DEPS := $(shell find rootfs -not -name '.gitignore' -type f | sort) \
               $(wildcard build/bin/*)

$(INITRD): $(INITRD_DEPS) | user-build
	@cd rootfs && find . -not -name '.gitignore' | sort | cpio -o --format=newc --owner=0:0 --reproducible > ../$@ 2>/dev/null
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
	cc $(LIMINE_DIR)/limine.c -o $@

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
	    -global qxl-vga.vgamem_mb=1024

run-serial: iso
	qemu-system-x86_64              \
	    -M q35                      \
	    -m 2G                       \
	    -cdrom $(ISO)               \
	    -boot d                     \
	    -display none               \
	    -serial stdio

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
	    -global qxl-vga.vgamem_mb=1024

TEST_ROOTFS := test_rootfs
TEST_INITRD := test-initrd.cpio
TEST_ISO    := kyronix-test.iso

testrunner: build/libatomic_asneeded.a
	$(MAKE) -C user/testrunner

test-initrd: $(TARGET) testrunner build/libatomic_asneeded.a
	$(MAKE) -C user/kyrobox
	$(MAKE) -C user/fetch
	rm -rf $(TEST_ROOTFS) $(TEST_INITRD)
	mkdir -p $(TEST_ROOTFS)/bin
	cp build/bin/testrunner $(TEST_ROOTFS)/init
	cp build/bin/ls        $(TEST_ROOTFS)/bin/
	cp build/bin/grep      $(TEST_ROOTFS)/bin/
	cp build/bin/echo      $(TEST_ROOTFS)/bin/
	cp build/bin/fetch     $(TEST_ROOTFS)/bin/
	cd $(TEST_ROOTFS) && find . | sort | cpio -o --format=newc --owner=0:0 --reproducible > ../$(TEST_INITRD) 2>/dev/null
	@echo "  Built: $(TEST_INITRD)"

test-iso: $(TARGET) test-initrd $(LIMINE_DIR)/limine
	rm -rf iso_root
	mkdir -p iso_root/boot/limine
	mkdir -p iso_root/EFI/BOOT
	cp $(TARGET)                    iso_root/boot/kernel.elf
	cp $(TEST_INITRD)               iso_root/boot/initrd.cpio
	cp limine.conf                  iso_root/boot/limine/limine.conf
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
	    iso_root -o $(TEST_ISO)
	./$(LIMINE_DIR)/limine bios-install $(TEST_ISO)
	@echo ""
	@echo "  Built: $(TEST_ISO)"

test-run: test-iso
	qemu-system-x86_64              \
	    -M q35                      \
	    -m 512M                     \
	    -cdrom $(TEST_ISO)          \
	    -display none               \
	    -serial stdio               \
	    -no-reboot

test-run-log: test-iso
	@qemu-system-x86_64              \
	    -M q35                      \
	    -m 512M                     \
	    -cdrom $(TEST_ISO)          \
	    -display none               \
	    -serial file:test.log       \
	    -no-reboot 2>/dev/null;     \
	grep -E "(TEST|RESULT|ALL|SOME)" test.log 2>/dev/null; \
	if grep -q "ALL TESTS PASSED" test.log 2>/dev/null; then \
	    echo ""; \
	    echo "PASS"; \
	else \
	    echo ""; \
	    echo "FAIL"; \
	    exit 1; \
	fi

fmt:
	@echo "Formatting code..."
	@find $(SRC_DIR) -type f \( -name "*.c" -o -name "*.h" \) -exec clang-format -i {} \;
	@echo "Format complete"

fmt-check:
	@echo "Checking code style..."
	@find $(SRC_DIR) -type f \( -name "*.c" -o -name "*.h" \) -exec clang-format --dry-run -Werror {} +

clean:
	rm -f $(TARGET) $(ISO) $(INITRD) $(TEST_ISO) $(TEST_INITRD)
	rm -rf $(BUILD_DIR) iso_root rootfs/bin $(TEST_ROOTFS)
	$(MAKE) -C user clean
	$(MAKE) -C $(LIMINE_DIR) clean 2>/dev/null; true
