CC      := musl-gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie
LDFLAGS ?= -static -no-pie
OUT     := ../../../build/bin

SRCS := main.c commands.c http.c md5.c util.c
OBJS := $(SRCS:.c=.o)

FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS += $(FNO_LINK_LIBATOMIC)

.PHONY: all clean

all: $(OUT)/pkg

$(OUT)/pkg: $(OBJS)
	mkdir -p $(OUT)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(OUT)/pkg
