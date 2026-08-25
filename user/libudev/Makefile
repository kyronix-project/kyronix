CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fPIC
LDFLAGS ?= -shared -Wl,-soname,libudev.so.1 -Wl,--version-script=libudev.map
OUT := ../../build/lib/libudev.so.1
ROOTFS := ../../rootfs

.PHONY: all clean install

all: $(OUT)

$(OUT): libudev.c libudev.map Makefile
	mkdir -p ../../build/lib
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ libudev.c
	@echo "  libudev: $(OUT)"

install: all
	mkdir -p $(ROOTFS)/usr/lib
	cp $(OUT) $(ROOTFS)/usr/lib/libudev.so.1
	ln -sf libudev.so.1 $(ROOTFS)/usr/lib/libudev.so

clean:
	rm -f $(OUT) $(ROOTFS)/usr/lib/libudev.so.1 $(ROOTFS)/usr/lib/libudev.so
