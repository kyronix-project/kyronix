CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie -L../../build
LDFLAGS ?= -static -no-pie
OUT := ../../build/bin
UTILS := awk basename cat chgrp chmod chown cksum clear cmp cp cut date dd diff dirname du echo env false \
	fdisk find grep head hostname kill killall less link ln ls mkdir mktemp mount mv nc nslookup ping printenv printf ps pwd readlink reboot rm rmdir \
	sed seq sleep sort stress sync tail tee test top touch tr true tty umount uname uniq unlink useradd userdel wc wget which whoami yes

FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS += $(FNO_LINK_LIBATOMIC)

.PHONY: all clean

all: $(UTILS:%=$(OUT)/%) $(OUT)/make

$(OUT)/%: %.c common.h
	mkdir -p ../../build/bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

$(OUT)/make: make/make
	mkdir -p $(OUT)
	cp $< $@

make/make: make/Makefile
	touch make/aclocal.m4 make/configure make/Makefile.in make/config.h.in
	$(MAKE) -C make

make/Makefile: make/configure
	cd make && ./configure CC="$(CC)" \
	    CFLAGS="-O2 -march=x86-64 -mtune=generic -fno-pie $(FNO_LINK_LIBATOMIC)" \
	    LDFLAGS="$(LDFLAGS)" \
	    --prefix=/usr --disable-nls

clean:
	rm -f $(UTILS:%=$(OUT)/%)
	$(MAKE) -C make clean 2>/dev/null; true
