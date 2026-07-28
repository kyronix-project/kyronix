TEST_INITRD := $(BUILD)/test-initrd.cpio
TEST_ROOTFS := $(BUILD)/test-rootfs
TEST_ISO_ROOT := $(BUILD)/test-iso-root
TEST_DISK := $(BUILD)/test-disk.img
TEST_LOG := $(BUILD)/test.log
TEST_TIMEOUT ?= 120
TEST_MODULE := $(BUILD)/hello.ko
TEST_DEP_MODULE := $(BUILD)/hello_user.ko

TESTRUNNER_SOURCES := $(wildcard user/testrunner/*.c user/testrunner/*.h) user/testrunner/Makefile

$(BUILD)/bin/testrunner: $(TESTRUNNER_SOURCES) $(BUILD)/libatomic_asneeded.a
	$(MAKE) -C user/testrunner

$(TEST_MODULE): user/testrunner/fixtures/modules/hello.c kernel/module.h kernel/version.h
	$(CC) $(CFLAGS) -fno-asynchronous-unwind-tables -c $< -o $@

$(TEST_DEP_MODULE): user/testrunner/fixtures/modules/dependent.c kernel/module.h kernel/version.h
	$(CC) $(CFLAGS) -fno-asynchronous-unwind-tables -c $< -o $@

$(TEST_INITRD): $(KERNEL) $(KERNEL_MODULES) $(USERSPACE_STAMP) $(BUILD)/bin/testrunner $(TEST_MODULE) $(TEST_DEP_MODULE)
	rm -rf $(TEST_ROOTFS)
	mkdir -p $(TEST_ROOTFS)/bin $(TEST_ROOTFS)/mnt $(TEST_ROOTFS)/lib/modules
	cp $(BUILD)/bin/testrunner $(TEST_ROOTFS)/init
	cp $(KERNEL_MODULES) $(TEST_ROOTFS)/lib/modules/
	cp $(TEST_MODULE) $(TEST_ROOTFS)/lib/modules/hello.ko
	cp $(TEST_DEP_MODULE) $(TEST_ROOTFS)/lib/modules/hello_user.ko
	cp $(BUILD)/bin/ksh $(TEST_ROOTFS)/bin/
	ln -sf ksh $(TEST_ROOTFS)/bin/sh
	for app in basename cat chgrp chmod chown cksum clear cmp cp cut date dd \
	    dirname du echo env false find grep head hostname kill killall less \
	    link ln ls mkdir mktemp mv nc nslookup ping printenv printf ps pwd \
	    readlink reboot rm rmdir sed seq sleep sort sync tail tee test touch \
	    tr true tty uname uniq unlink wc wget which whoami yes; do \
	    cp $(BUILD)/bin/$$app $(TEST_ROOTFS)/bin/; \
	done
	for bin in tar gzip gunzip bzip2 bunzip2 xz unxz; do \
	    cp -P $(BUILD)/bin/$$bin $(TEST_ROOTFS)/bin/; \
	done
	@cd $(TEST_ROOTFS) && find . | sort | \
	    cpio -o --format=newc --owner=0:0 --reproducible \
	    > $(abspath $(TEST_INITRD)) 2>/dev/null
	@echo "  Built: $(TEST_INITRD)"

$(TEST_ISO): $(KERNEL) $(TEST_INITRD) limine-test.conf \
		$(LIMINE_FILES) $(LIMINE)/limine
	@mkdir -p $(@D)
	$(call build_iso,$(TEST_ISO_ROOT),limine-test.conf,$(TEST_INITRD),$(TEST_ISO))

.PHONY: test
test: $(TEST_ISO)
	@mkdir -p $(BUILD)
	truncate -s 16M $(TEST_DISK)
	mkfs.ext2 -F -b 4096 -L kyronix-test $(TEST_DISK) >/dev/null 2>&1
	@set +e; \
	timeout $(TEST_TIMEOUT) $(QEMU) \
	    -M q35 -cpu max -m 512M -smp 4 \
	    -cdrom $(TEST_ISO) \
	    -device isa-debug-exit,iobase=0x501 \
	    -display none \
	    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
	    -drive file=$(TEST_DISK),format=raw,if=none,id=hd0,cache=writethrough \
	    -device ahci,id=ahci \
	    -device ide-hd,drive=hd0,bus=ahci.0 \
	    -serial file:$(TEST_LOG) -no-reboot 2>/dev/null; \
	qemu_status=$$?; \
	set -e; \
	grep -E "(TEST|RESULT|ALL|SOME|KMEMLEAK)" $(TEST_LOG) 2>/dev/null || true; \
	if ! grep -q "ALL TESTS PASSED" $(TEST_LOG) 2>/dev/null; then \
	    echo; echo "FAIL (QEMU status $$qemu_status)"; exit 1; \
	fi; \
	leaks=$$(sed -n 's/.*KMEMLEAK: \\([0-9][0-9]*\\).*/\\1/p' $(TEST_LOG) | tail -n 1); \
	if [ -n "$$leaks" ] && [ "$$leaks" -gt 0 ]; then \
	    echo; echo "FAIL: KMEMLEAK found $$leaks leak(s)"; exit 1; \
	fi; \
	echo; echo "PASS"
