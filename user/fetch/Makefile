CC := musl-gcc
CFLAGS := -std=c99 -O2 -Wall -Wextra -fno-pie -L../../build
LDFLAGS := -static -no-pie
OUT := ../../build/bin

FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS += $(FNO_LINK_LIBATOMIC)

.PHONY: all clean

all: $(OUT)/fetch

$(OUT)/fetch: fetch.c
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(OUT)/fetch
