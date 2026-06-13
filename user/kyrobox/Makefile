CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie -L../../build
LDFLAGS ?= -static -no-pie
OUT := ../../build/bin
UTILS := basename cat chgrp chmod chown cksum clear cmp cp cut date dd dirname du echo env false \
	find grep head hostname kill link ln ls mkdir mktemp mv printenv printf pwd readlink rm rmdir \
	sed seq sleep sort sync tail tee test touch tr true tty uname uniq unlink wc which whoami yes

.PHONY: all clean

all: $(UTILS:%=$(OUT)/%)

$(OUT)/%: %.c common.h
	mkdir -p ../../build/bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(UTILS:%=$(OUT)/%)
