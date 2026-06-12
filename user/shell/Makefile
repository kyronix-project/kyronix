CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie -L../../build
LDFLAGS ?= -static -no-pie
OUT := ../../build/bin/ksh

.PHONY: all check-musl clean

all: check-musl $(OUT)

check-musl:
	@command -v $(CC) >/dev/null 2>&1 && test -f /usr/lib/musl/lib/libc.a || { \
		exit 1; \
	}

$(OUT): ksh.c
	mkdir -p ../../build/bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(OUT)
