ROOTFS := ../rootfs

.PHONY: all clean

all:
	$(MAKE) -C init
	$(MAKE) -C shell
	$(MAKE) -C login
	$(MAKE) -C vi
	$(MAKE) -C kyrobox
	$(MAKE) -C pkg/client
	$(MAKE) -C phantom-test
	$(MAKE) -C jitter-test
	mkdir -p $(ROOTFS)/bin
	cp ../build/bin/kyronix-init $(ROOTFS)/bin/init
	cp ../build/bin/ksh $(ROOTFS)/bin/
	cp ../build/bin/login $(ROOTFS)/bin/
	cp ../build/bin/vi $(ROOTFS)/bin/
	cp ../build/bin/make $(ROOTFS)/bin/
	cp ../build/bin/pkg $(ROOTFS)/bin/
	cp ../build/bin/phantom-test $(ROOTFS)/bin/
	cp ../build/bin/jitter-test $(ROOTFS)/bin/
	ln -sf ksh $(ROOTFS)/bin/sh
	for app in awk basename cat chgrp chmod chown cksum clear cmp cp cut date dd diff dirname du echo env false \
	    fdisk find grep head hostname kill killall less link ln ls mkdir mktemp mount mv nc nslookup ping printenv printf ps pwd readlink reboot rm rmdir \
	    sed seq sleep sort sync tail tee test top touch tr true tty umount uname uniq unlink wc wget which whoami yes; do \
	    cp ../build/bin/$$app $(ROOTFS)/bin/; \
	done
	ln -sf test $(ROOTFS)/bin/[
	ln -sf reboot $(ROOTFS)/bin/poweroff
	ln -sf reboot $(ROOTFS)/bin/halt
	$(MAKE) -C extras install

clean:
	$(MAKE) -C init clean
	$(MAKE) -C shell clean
	$(MAKE) -C login clean
	$(MAKE) -C vi clean
	$(MAKE) -C kyrobox clean
	$(MAKE) -C pkg/client clean
	$(MAKE) -C phantom-test clean
	$(MAKE) -C jitter-test clean
	$(MAKE) -C extras clean
	rm -f $(ROOTFS)/bin/*
