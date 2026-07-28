CC := musl-gcc
ROOTFS := ../../rootfs
OUT_DIR := ../../build/bin
INSTALLER := $(OUT_DIR)/installer
MKFS := $(OUT_DIR)/mkfs.ext2
LIMINE_INSTALL := $(OUT_DIR)/limine-install

CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic
NCURSES_CFLAGS := -I/usr/include/ncursesw -idirafter /usr/include
NCURSES_LIB := $(ROOTFS)/usr/lib/libncursesw.so.6

.PHONY: all clean

all: $(INSTALLER) $(MKFS) $(LIMINE_INSTALL)

$(OUT_DIR):
	mkdir -p $@

$(INSTALLER): installer.c $(NCURSES_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) $(NCURSES_CFLAGS) -no-pie \
	    -Wl,--allow-shlib-undefined -o $@ $< $(NCURSES_LIB)

$(MKFS): mkfs_ext2.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -static -no-pie -o $@ $<

$(LIMINE_INSTALL): ../../limine/limine.c | $(OUT_DIR)
	$(CC) -std=c11 -O2 -static -no-pie -o $@ $<

clean:
	rm -f $(INSTALLER) $(MKFS) $(LIMINE_INSTALL)
