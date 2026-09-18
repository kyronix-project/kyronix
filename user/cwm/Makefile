CC := musl-gcc
FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS := -std=gnu11 -Wall -O2 -march=x86-64 -mtune=generic -fno-pie $(FNO_LINK_LIBATOMIC) -idirafter /usr/include
CFLAGS += $(shell pkg-config --cflags x11 xft xrandr) -I.
LDFLAGS := -no-pie -Wl,--allow-shlib-undefined -Wl,--export-dynamic \
  ../../rootfs/usr/lib/libX11.so.6 \
  ../../rootfs/usr/lib/libXft.so.2 \
  ../../rootfs/usr/lib/libXrandr.so.2 \
  ../../rootfs/usr/lib/libxcb.so.1 \
  ../../rootfs/usr/lib/libXau.so.6 \
  ../../rootfs/usr/lib/libXdmcp.so.6 \
  ../../rootfs/usr/lib/libXrender.so.1 \
  ../../rootfs/usr/lib/libXext.so.6 \
  ../../rootfs/usr/lib/libfontconfig.so.1 \
  ../../rootfs/usr/lib/libfreetype.so.6 \
  ../../rootfs/usr/lib/libexpat.so.1 \
  ../../rootfs/usr/lib/libpng16.so.16 \
  ../../rootfs/usr/lib/libbz2.so.1 \
  ../../rootfs/usr/lib/libz.so.1 \
  ../../rootfs/usr/lib/libbrotlidec.so.1 \
  ../../rootfs/usr/lib/libbrotlicommon.so.1 \
  ../../rootfs/usr/lib/libmd.so.0 \
  ../../rootfs/usr/lib/libbsd.so.0
OUT := ../../build/bin
SRCS := calmwm.c screen.c xmalloc.c client.c menu.c search.c \
        util.c xutil.c conf.c xevents.c group.c kbfunc.c \
        strlcpy.c strlcat.c strtonum.c reallocarray.c compat.c
PARSE := parse.c
PROG := cwm

.PHONY: all clean

all: $(OUT)/$(PROG)

$(OUT)/$(PROG): $(SRCS) $(PARSE)
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(PARSE)

parse.c: parse.y
	bison -o $@ $<

clean:
	rm -f $(OUT)/$(PROG) parse.c
