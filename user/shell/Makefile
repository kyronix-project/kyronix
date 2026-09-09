CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie -L../../build
LDFLAGS ?= -static -no-pie
OUT := ../../build/bin/ksh

FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS += $(FNO_LINK_LIBATOMIC)

include ../../mk/output.mk

.PHONY: all check-musl clean

all: check-musl $(OUT)

check-musl:
	@command -v $(CC) >/dev/null 2>&1 || { echo "musl-gcc not found"; exit 1; }

$(OUT): ksh.c
	@$(call step,CC $<)
	@mkdir -p ../../build/bin
	@$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(OUT)
