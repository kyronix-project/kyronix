CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie
LDFLAGS ?= -static -no-pie -pthread
OUT := ../../build/bin/jitter-test

.PHONY: all clean
all: $(OUT)

$(OUT): jitter-test.c
	mkdir -p ../../build/bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(OUT)
