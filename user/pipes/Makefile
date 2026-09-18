CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie
LDFLAGS ?= -static -no-pie
OUT := ../../build/bin/pipes

FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS += $(FNO_LINK_LIBATOMIC)

$(OUT): pipes.c
	mkdir -p ../../build/bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(OUT)

.PHONY: all clean
all: $(OUT)
