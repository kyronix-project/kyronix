CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie -L../../build
LDFLAGS ?= -static -no-pie
OUT := ../../build/bin
UTILS := awk basename cat chgrp chmod chown cksum clear cmp cp cut date dd diff dirname du echo env false \
	fdisk file find grep head hostname id insmod kill killall less link ln ls lsmod mkdir mktemp mount mv nc nslookup ntpd patch ping printenv printf ps pwd readlink realpath \
	reboot rm rmdir rmmod sed seq sleep sort stat stress sync tail tee test top touch tr true tty umount uname uniq unlink useradd userdel wc wget which whoami xargs yes tree

FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS += $(FNO_LINK_LIBATOMIC)

.PHONY: all clean

all: $(UTILS:%=$(OUT)/%)

$(OUT)/%: %.c common.h
	mkdir -p ../../build/bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(UTILS:%=$(OUT)/%)
