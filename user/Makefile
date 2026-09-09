ROOTFS := ../rootfs

include ../mk/output.mk

.PHONY: all clean

all:
	@$(MAKE) --no-print-directory -C init
	@$(MAKE) --no-print-directory -C logctl
	@$(MAKE) --no-print-directory -C servctl
	@$(MAKE) --no-print-directory -C jailctl
	@$(MAKE) --no-print-directory -C shell
	@$(MAKE) --no-print-directory -C login
	@$(MAKE) --no-print-directory -C vi
	@$(MAKE) --no-print-directory -C kyrobox
	@$(MAKE) --no-print-directory -C pkg/client
	@$(MAKE) --no-print-directory -C installer
	@$(MAKE) --no-print-directory -C libudev
	@mkdir -p $(ROOTFS)/bin
	@cp ../build/bin/kyronix-init $(ROOTFS)/bin/init
	@rm -f $(ROOTFS)/bin/logger $(ROOTFS)/bin/sv
	@cp ../build/bin/logctl $(ROOTFS)/bin/
	@cp ../build/bin/servctl $(ROOTFS)/bin/
	@cp ../build/bin/ksh $(ROOTFS)/bin/
	@cp ../build/bin/login $(ROOTFS)/bin/
	@cp ../build/bin/vi $(ROOTFS)/bin/
	@cp ../build/bin/pkg $(ROOTFS)/bin/
	@cp ../build/bin/installer $(ROOTFS)/bin/
	@cp ../build/bin/mkfs.ext2 $(ROOTFS)/bin/
	@mkdir -p $(ROOTFS)/usr/libexec
	@cp ../build/bin/limine-install $(ROOTFS)/usr/libexec/
	@ln -sf ksh $(ROOTFS)/bin/sh
	@for app in awk basename cat chgrp chmod chown cksum clear cmp cp cut date dd diff dirname du echo env false \
	    fdisk find file grep head hostname id insmod jailctl kill killall less link ln ls lsblk lspci lsmod mkdir mktemp mount mv nc nslookup ntpd patch ping printenv printf ps pwd readlink realpath \
	    passwd reboot rm rmdir rmmod sed seq sleep sort stat stress sync tail tee test top touch tr true tty tree umount uname uniq unlink useradd userdel wc wget which whoami xargs yes; do \
	    cp ../build/bin/$$app $(ROOTFS)/bin/; \
	done
	@chmod 4755 $(ROOTFS)/bin/passwd
	@chmod 0755 $(ROOTFS)/etc
	@chmod 0600 $(ROOTFS)/etc/shadow
	@ln -sf test $(ROOTFS)/bin/[
	@ln -sf reboot $(ROOTFS)/bin/poweroff
	@ln -sf reboot $(ROOTFS)/bin/halt
	@$(MAKE) --no-print-directory -C libudev install
	@$(MAKE) --no-print-directory -C extras install
ifneq ($(WAYLAND),0)
	@$(MAKE) --no-print-directory -C extras/blobs/wayland install
	@$(MAKE) --no-print-directory -C extras/blobs/gtk
	@$(MAKE) --no-print-directory -C panel install
endif

clean:
	@$(MAKE) --no-print-directory -C init clean
	@$(MAKE) --no-print-directory -C logctl clean
	@$(MAKE) --no-print-directory -C servctl clean
	@$(MAKE) --no-print-directory -C jailctl clean
	@$(MAKE) --no-print-directory -C shell clean
	@$(MAKE) --no-print-directory -C login clean
	@$(MAKE) --no-print-directory -C vi clean
	@$(MAKE) --no-print-directory -C kyrobox clean
	@$(MAKE) --no-print-directory -C pkg/client clean
	@$(MAKE) --no-print-directory -C installer clean
	@$(MAKE) --no-print-directory -C extras clean
	@$(MAKE) --no-print-directory -C libudev clean
	@$(MAKE) --no-print-directory -C panel clean
	@$(MAKE) --no-print-directory -C extras/blobs/wayland clean
	@rm -f $(ROOTFS)/bin/*
	@rm -f $(ROOTFS)/usr/libexec/limine-install
