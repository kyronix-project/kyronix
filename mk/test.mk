TEST_INITRD := $(BUILD)/test-initrd.cpio
TEST_ROOTFS := $(BUILD)/test-rootfs
TEST_ISO_ROOT := $(BUILD)/test-iso-root
TEST_DISK := $(BUILD)/test-disk.img
TEST_SQSH := $(BUILD)/test.sqsh
TEST_SQSH_XZ := $(BUILD)/test-xz.sqsh
TEST_SQFS_SRC := $(BUILD)/sqfs-src
TEST_LOG := $(BUILD)/test.log
TEST_TIMEOUT ?= 600
TEST_MODULE := $(BUILD)/hello.ko
TEST_DEP_MODULE := $(BUILD)/hello_user.ko
FUZZ_ISO := $(DIST)/kyronix-posix-fuzz.iso
FUZZ_INITRD := $(BUILD)/posix-fuzz-initrd.cpio
FUZZ_ROOTFS := $(BUILD)/posix-fuzz-rootfs
FUZZ_ISO_ROOT := $(BUILD)/posix-fuzz-iso-root
FUZZ_LOG := $(BUILD)/posix-fuzz.log
FUZZ_SEED ?= 1
FUZZ_CASES ?= 140
FUZZ_ONLY ?= -1
FUZZ_TIMEOUT ?= 120

.PHONY: fuzz-posix

$(BUILD)/bin/posix-fuzz: user/posix_fuzz/posix_fuzz.c
	@mkdir -p $(BUILD)/bin
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 -static -fno-pie \
	    -DKYRONIX_GUEST -DFUZZ_SEED=$(FUZZ_SEED) -DFUZZ_CASES=$(FUZZ_CASES) \
	    -DFUZZ_ONLY=$(FUZZ_ONLY) \
	    user/posix_fuzz/posix_fuzz.c -o $@

$(FUZZ_INITRD): $(BUILD)/bin/posix-fuzz
	@mkdir -p $(FUZZ_ROOTFS)/tmp
	@cp $(BUILD)/bin/posix-fuzz $(FUZZ_ROOTFS)/init
	@cd $(FUZZ_ROOTFS) && find . | sort | \
	    cpio -o --format=newc --owner=0:0 --reproducible \
	    > "$(abspath $(FUZZ_INITRD))" 2>/dev/null

$(FUZZ_ISO): $(KERNEL) $(FUZZ_INITRD) boot/limine-test.conf $(LIMINE_FILES) $(LIMINE)/limine
	@mkdir -p $(@D)
	$(call build_iso,$(FUZZ_ISO_ROOT),boot/limine-test.conf,$(FUZZ_INITRD),,$(FUZZ_ISO))

fuzz-posix: $(FUZZ_ISO)
	@mkdir -p $(BUILD)
	@set +e; timeout $(FUZZ_TIMEOUT) $(QEMU) \
	    -M q35 -cpu max -m 512M -smp 2 -cdrom $(FUZZ_ISO) \
	    -device isa-debug-exit,iobase=0x501 -display none \
	    -serial file:$(FUZZ_LOG) -no-reboot 2>/dev/null; \
	qemu_status=$$?; set -e; \
	python3 scripts/posix_fuzz_report.py $(FUZZ_LOG) \
	    --json $(BUILD)/posix-fuzz-report.json \
	    --markdown $(BUILD)/posix-fuzz-report.md \
	    --qemu-status $$qemu_status

TESTRUNNER_SOURCES := $(wildcard user/testrunner/*.c user/testrunner/*.h) user/testrunner/Makefile

$(BUILD)/bin/testrunner: $(TESTRUNNER_SOURCES) $(BUILD)/libatomic_asneeded.a
	$(MAKE) -C user/testrunner

$(TEST_MODULE): user/testrunner/fixtures/modules/hello.c kernel/module.h kernel/version.h
	@mkdir -p $(@D)
	@$(call step,CC $<)
	@$(CC) $(CFLAGS) -fno-asynchronous-unwind-tables -c $< -o $@

$(TEST_DEP_MODULE): user/testrunner/fixtures/modules/dependent.c kernel/module.h kernel/version.h
	@mkdir -p $(@D)
	@$(call step,CC $<)
	@$(CC) $(CFLAGS) -fno-asynchronous-unwind-tables -c $< -o $@

SQFS_COMP ?= gzip
SQFS_OPTS := -noappend -no-progress -comp $(SQFS_COMP) -b 131072 -no-exports

ifeq ($(strip $(INSIDE_CONTAINER)),1)
MKSQ := mksquashfs $(TEST_SQFS_SRC) $(TEST_SQSH) $(SQFS_OPTS) >/dev/null
MKSQ_XZ := mksquashfs $(TEST_SQFS_SRC) $(TEST_SQSH_XZ) -noappend -no-progress -comp xz -b 131072 -no-exports >/dev/null
else
MKSQ := $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
    sh -c "mksquashfs /src/$(TEST_SQFS_SRC) /src/$(TEST_SQSH) $(SQFS_OPTS)" >/dev/null
MKSQ_XZ := $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
    sh -c "mksquashfs /src/$(TEST_SQFS_SRC) /src/$(TEST_SQSH_XZ) -noappend -no-progress -comp xz -b 131072 -no-exports" >/dev/null
endif

$(TEST_SQFS_SRC):
	@$(call phase,Squashfs fixture)
	rm -rf $(TEST_SQFS_SRC)
	mkdir -p $(TEST_SQFS_SRC)/bin $(TEST_SQFS_SRC)/usr/share/doc
	printf 'hello from squashfs\n' > $(TEST_SQFS_SRC)/hello.txt
	printf '#!/bin/sh\necho AppRun executed\necho "APPDIR=$$APPDIR"\necho "APPIMAGE=$$APPIMAGE"\necho "ARG1=$$1"\nexit $$AI_EXIT\n' > $(TEST_SQFS_SRC)/AppRun
	chmod 755 $(TEST_SQFS_SRC)/AppRun
	printf 'docs\n' > $(TEST_SQFS_SRC)/usr/share/doc/readme.md
	python3 -c "open('$(TEST_SQFS_SRC)/big.bin','wb').write(bytes((i*31+7)&0xFF for i in range(300000)))"
	python3 -c "open('$(TEST_SQFS_SRC)/bin/tool.dat','wb').write(bytes((i*7+3)&0xFF for i in range(3000)))"
	python3 -c "d=bytearray(1024*1024); [d.__setitem__(i,(i*13+1)&0xFF) for i in range(1024)]; open('$(TEST_SQFS_SRC)/sparse.bin','wb').write(d)"
	ln -sf hello.txt $(TEST_SQFS_SRC)/link.txt
	ln -sf ../hello.txt $(TEST_SQFS_SRC)/bin/up.txt

$(TEST_SQSH): $(TEST_SQFS_SRC)
	$(MKSQ)
	@$(call ok,$(TEST_SQSH))

$(TEST_SQSH_XZ): $(TEST_SQFS_SRC)
	$(MKSQ_XZ)
	@$(call ok,$(TEST_SQSH_XZ))

# A script AppRun with no libs/, which is the shape real AppImages have. The
# script is the same construct neovim uses, so the test covers dirname and
# readlink -f resolving inside the chroot; usr/bin/prog is the glibc fixture
# binary, which only loads where the sysroot provides its interpreter.
BASH_APP_STAGE := $(BUILD)/bash-app-stage
BASH_APP_SQSH := $(BUILD)/bash-app.sqsh
BASH_APPIMG := $(BUILD)/bash-app.img

$(BASH_APP_STAGE): $(REAL_APP_STAGE)/.stamp
	@$(call phase,Script AppRun fixture)
	rm -rf $(BASH_APP_STAGE)
	mkdir -p $(BASH_APP_STAGE)/usr/bin
	cp $(REAL_APP_STAGE)/AppRun $(BASH_APP_STAGE)/usr/bin/prog
	printf '#!/bin/bash\nunset ARGV0\necho "bash-apprun ok"\necho "apdir=$$(dirname "$$(readlink -f "$${0}")")"\nexec "$$(dirname "$$(readlink -f "$${0}")")/usr/bin/prog" $${@+"$$@"}\n' > $(BASH_APP_STAGE)/AppRun
	chmod 755 $(BASH_APP_STAGE)/AppRun

$(BASH_APP_SQSH): $(BASH_APP_STAGE)
	@$(call MKSQUASHFS,$(BASH_APP_STAGE),$(BASH_APP_SQSH))

$(BASH_APPIMG): $(BASH_APP_SQSH) scripts/wrap_appimages.py
	@python3 $(CURDIR)/scripts/wrap_appimages.py --single $(BASH_APP_SQSH) $(BASH_APPIMG) >/dev/null
	@$(call ok,$(BASH_APPIMG))

$(TEST_INITRD): $(KERNEL) $(KERNEL_MODULES) $(USERSPACE_STAMP) $(BUILD)/bin/testrunner $(TEST_MODULE) $(TEST_DEP_MODULE) $(TEST_SQSH) $(TEST_SQSH_XZ) $(GLIBC_STAMP) $(REAL_APPIMG) $(BASH_STAMP) $(BASH_APPIMG) $(NVIM_STAMP) $(WL_PROBEIMG)
	@$(call phase,Test initrd)
	rm -rf $(TEST_ROOTFS)
	mkdir -p $(TEST_ROOTFS)/bin $(TEST_ROOTFS)/mnt $(TEST_ROOTFS)/sqfs \
	    $(TEST_ROOTFS)/etc $(TEST_ROOTFS)/lib/modules
	printf 'sdb /sqfs squashfs ro 0 0\n' > $(TEST_ROOTFS)/etc/fstab
	@# kair binds the host's /etc/terminfo into the sandbox; TERM=xterm-color is
	@# not in neovim's builtin table, so the guest has to carry the real entries.
	@mkdir -p $(TEST_ROOTFS)/etc/terminfo
	@cp -a rootfs/etc/terminfo/. $(TEST_ROOTFS)/etc/terminfo/
	python3 scripts/wrap_appimages.py '$(TEST_ROOTFS)' '$(TEST_SQSH)' '$(TEST_SQSH_XZ)'
	@if [ -f $(REAL_APPIMG) ]; then \
	    cp $(REAL_APPIMG) $(TEST_ROOTFS)/real_app.img; \
	else \
	    echo "  [WARN] no real AppImage fixture; real-appimage tests will skip"; \
	fi
	@if [ -f $(BASH_APPIMG) ]; then \
	    cp $(BASH_APPIMG) $(TEST_ROOTFS)/bash_app.img; \
	else \
	    echo "  [WARN] no script-AppRun fixture; bash-appimage tests will skip"; \
	fi
	@if [ -s $(NVIM_APPIMG) ]; then \
	    cp $(NVIM_APPIMG) $(TEST_ROOTFS)/nvim.appimage; \
	else \
	    echo "  [WARN] no neovim AppImage; real-app tests will skip"; \
	fi
	@# The Wayland client goes through the sysroot, not the image: it has no
	@# libs/ of its own, so its libwayland-client comes from the glibc sysroot.
	@if [ -f $(WL_PROBEIMG) ]; then \
	    cp $(WL_PROBEIMG) $(TEST_ROOTFS)/wl_probe.img; \
	else \
	    echo "  [WARN] no Wayland client fixture; wayland tests will skip"; \
	fi
	mkdir -p $(TEST_ROOTFS)/glibc $(TEST_ROOTFS)/lib64
	cp $(GLIBC_FIXTURES)/hello-glibc $(GLIBC_FIXTURES)/libc.so.6 $(GLIBC_FIXTURES)/libm.so.6 $(TEST_ROOTFS)/glibc/
	cp $(GLIBC_FIXTURES)/ld-linux-x86-64.so.2 $(TEST_ROOTFS)/lib64/
	@$(call phase,Test runtime sysroot)
	@printf 'kair-runtime\n' > $(BUILD)/test-packages.list
	@./scripts/local-pkg-install --registry ./pkg-registry-src \
	    --target $(abspath $(TEST_ROOTFS)) --list $(BUILD)/test-packages.list
	@if [ -f $(TEST_ROOTFS)/usr/lib/sysroots/debian-runtime.sqsh ]; then \
	    $(call ok,glibc sysroot from kair-runtime); \
	else \
	    $(call warn,no glibc sysroot - AppImage/glibc tests will SKIP, not fail); \
	fi
	cp $(BUILD)/bin/testrunner $(TEST_ROOTFS)/init
	cp $(KERNEL_MODULES) $(TEST_ROOTFS)/lib/modules/
	cp $(TEST_MODULE) $(TEST_ROOTFS)/lib/modules/hello.ko
	cp $(TEST_DEP_MODULE) $(TEST_ROOTFS)/lib/modules/hello_user.ko
	cp $(BUILD)/bin/ksh $(TEST_ROOTFS)/bin/
	ln -sf ksh $(TEST_ROOTFS)/bin/sh
	for app in basename cat chgrp chmod chown chroot cksum clear cmp cp cut date dd \
	    dirname du echo env false find grep head hostname kair kill killall less \
	    link ln ls mkdir mktemp mount mv nc nslookup ping printenv printf ps pwd \
	    readlink reboot rm rmdir sed seq sleep sort sync tail tee test touch \
	    tr true tty umount unshare uname uniq unlink wc wget which whoami yes; do \
	    cp $(BUILD)/bin/$$app $(TEST_ROOTFS)/bin/; \
	done
	@if [ -s $(BUILD)/bin/bash ]; then cp $(BUILD)/bin/bash $(TEST_ROOTFS)/bin/; \
	else echo "  [WARN] no static bash; script AppRun tests will skip"; fi
	for bin in tar gzip gunzip bzip2 bunzip2 xz unxz; do \
	    cp -P $(BUILD)/bin/$$bin $(TEST_ROOTFS)/bin/; \
	done
	@cd $(TEST_ROOTFS) && find . | sort | \
	    cpio -o --format=newc --owner=0:0 --reproducible \
	    > "$(abspath $(TEST_INITRD))" 2>/dev/null
	@$(call ok,$(TEST_INITRD))

$(TEST_ISO): $(KERNEL) $(TEST_INITRD) boot/limine-test.conf \
		$(LIMINE_FILES) $(LIMINE)/limine
	@mkdir -p $(@D)
	$(call build_iso,$(TEST_ISO_ROOT),boot/limine-test.conf,$(TEST_INITRD),,$(TEST_ISO))

.PHONY: test
test: INSTRUMENT=1
test: $(TEST_ISO) $(TEST_SQSH)
	@$(call phase,Test suite)
	@mkdir -p $(BUILD)
	@truncate -s 16M $(TEST_DISK)
	@mkfs.ext2 -F -b 4096 -L kyronix-test $(TEST_DISK) >/dev/null 2>&1
	@set +e; \
	timeout $(TEST_TIMEOUT) $(QEMU) \
	    -M q35 -cpu max -m 2G -smp 4 \
	    -cdrom $(TEST_ISO) \
	    -device isa-debug-exit,iobase=0x501 \
	    -display none \
	    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
	    -drive file=$(TEST_DISK),format=raw,if=none,id=hd0,cache=writethrough \
	    -device ahci,id=ahci \
	    -device ide-hd,drive=hd0,bus=ahci.0 \
	    -drive file=$(TEST_SQSH),format=raw,if=none,id=hd1,cache=writethrough \
	    -device ide-hd,drive=hd1,bus=ahci.1 \
	    -serial file:$(TEST_LOG) -no-reboot 2>/dev/null; \
	qemu_status=$$?; \
	set -e; \
	grep -E "(TEST|RESULT|ALL|SOME|FAIL|KMEMLEAK)" $(TEST_LOG) 2>/dev/null || true; \
	if ! grep -q "ALL TESTS PASSED" $(TEST_LOG) 2>/dev/null; then \
	    echo; $(call fail,QEMU status $$qemu_status); exit 1; \
	fi; \
	leaks=$$(sed -n 's/.*KMEMLEAK: \\([0-9][0-9]*\\).*/\\1/p' $(TEST_LOG) | tail -n 1); \
	if [ -n "$$leaks" ] && [ "$$leaks" -gt 0 ]; then \
	    echo; $(call fail,KMEMLEAK found $$leaks leak(s)); exit 1; \
	fi; \
	echo; $(call ok,All tests passed)
