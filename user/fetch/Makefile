CC := musl-gcc
CFLAGS := -std=c99 -O2 -Wall -Wextra -fno-pie -L../../build
LDFLAGS := -static -no-pie
OUT := ../../build/bin

.PHONY: all clean

all: $(OUT)/fetch

$(OUT)/fetch: fetch.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(OUT)/fetch
