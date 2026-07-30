ROOTFS := ../rootfs

.PHONY: all clean

all:
	$(MAKE) -C init
	$(MAKE) -C logger
	$(MAKE) -C shell
	$(MAKE) -C login
	$(MAKE) -C vi
	$(MAKE) -C kyrobox
	$(MAKE) -C pkg/client
	$(MAKE) -C installer
	mkdir -p $(ROOTFS)/bin
	cp ../build/bin/kyronix-init $(ROOTFS)/bin/init
	cp ../build/bin/logger $(ROOTFS)/bin/
	cp ../build/bin/ksh $(ROOTFS)/bin/
	cp ../build/bin/login $(ROOTFS)/bin/
	cp ../build/bin/vi $(ROOTFS)/bin/
	cp ../build/bin/pkg $(ROOTFS)/bin/
	cp ../build/bin/installer $(ROOTFS)/bin/
	cp ../build/bin/mkfs.ext2 $(ROOTFS)/bin/
	mkdir -p $(ROOTFS)/usr/libexec
	cp ../build/bin/limine-install $(ROOTFS)/usr/libexec/
	ln -sf ksh $(ROOTFS)/bin/sh
	for app in awk basename cat chgrp chmod chown cksum clear cmp cp cut date dd diff dirname du echo env false \
	    fdisk find file grep head hostname id insmod kill killall less link ln ls lsmod mkdir mktemp mount mv nc nslookup ntpd patch ping printenv printf ps pwd readlink realpath \
	    reboot rm rmdir rmmod sed seq sleep sort stat stress sync tail tee test top touch tr true tty tree umount uname uniq unlink useradd userdel wc wget which whoami xargs yes; do \
	    cp ../build/bin/$$app $(ROOTFS)/bin/; \
	done
	ln -sf test $(ROOTFS)/bin/[
	ln -sf reboot $(ROOTFS)/bin/poweroff
	ln -sf reboot $(ROOTFS)/bin/halt
	$(MAKE) -C extras install

clean:
	$(MAKE) -C init clean
	$(MAKE) -C logger clean
	$(MAKE) -C shell clean
	$(MAKE) -C login clean
	$(MAKE) -C vi clean
	$(MAKE) -C kyrobox clean
	$(MAKE) -C pkg/client clean
	$(MAKE) -C installer clean
	$(MAKE) -C extras clean
	rm -f $(ROOTFS)/bin/*
	rm -f $(ROOTFS)/usr/libexec/limine-install
