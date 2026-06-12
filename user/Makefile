ROOTFS := ../rootfs

.PHONY: all clean

all:
	$(MAKE) -C init
	$(MAKE) -C shell
	$(MAKE) -C login
	$(MAKE) -C vi
	$(MAKE) -C kyrobox
	$(MAKE) -C fetch
	mkdir -p ../build/bin
	musl-gcc -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie -fno-link-libatomic -static -no-pie -o ../build/bin/pipetest pipetest.c
	rm -rf $(ROOTFS)/bin
	mkdir -p $(ROOTFS)/bin
	cp ../build/bin/kyronix-init $(ROOTFS)/bin/init
	cp ../build/bin/ksh $(ROOTFS)/bin/
	cp ../build/bin/login $(ROOTFS)/bin/
	cp ../build/bin/vi $(ROOTFS)/bin/
	cp ../build/bin/fetch $(ROOTFS)/bin/
	cp ../build/bin/pipetest $(ROOTFS)/bin/
	ln -sf ksh $(ROOTFS)/bin/sh
	for app in basename cat chgrp chmod chown cksum clear cmp cp cut date dd dirname du echo env false \
	    find grep head hostname kill link ln ls mkdir mktemp mv printenv printf pwd readlink rm rmdir \
	    sed seq sleep sort sync tail tee test touch tr true tty uname uniq unlink wc which whoami yes; do \
	    cp ../build/bin/$$app $(ROOTFS)/bin/; \
	done

clean:
	$(MAKE) -C init clean
	$(MAKE) -C shell clean
	$(MAKE) -C login clean
	$(MAKE) -C vi clean
	$(MAKE) -C kyrobox clean
	$(MAKE) -C fetch clean
	rm -rf $(ROOTFS)/bin
