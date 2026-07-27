CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie
LDFLAGS ?= -static -no-pie
OUT := ../../build/bin/phantom-test

.PHONY: all clean
all: $(OUT)

$(OUT): phantom-test.c
	mkdir -p ../../build/bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(OUT)
