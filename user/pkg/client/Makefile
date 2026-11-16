CC      := gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2 -D_FORTIFY_SOURCE=2 \
	-fstack-protector-strong -march=x86-64 -mtune=generic -fno-pie
LDFLAGS ?= -static -Wl,-z,noexecstack
OUT     := ../../../build/bin

SRCS := main.c commands.c http.c md5.c util.c zstd_decode.c
OBJS := $(SRCS:.c=.o)

FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS += $(FNO_LINK_LIBATOMIC)

include ../../../mk/output.mk

.PHONY: all clean

all: $(OUT)/pkg

$(OUT)/pkg: $(OBJS)
	@$(call step,LD $@)
	@mkdir -p $(OUT)
	@$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	@$(call step,CC $<)
	@$(CC) $(CFLAGS) -c -o $@ $<

zstd_decode.o: ../../../kernel/lib/zstd/zstddeclib.c zstd_decode.h
commands.o: zstd_decode.h

clean:
	rm -f $(OBJS) $(OUT)/pkg
