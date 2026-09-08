# Kyronix GTK3 panel.
#
# Built dynamically against the GTK3 runtime installed from gtk/ into
# /usr/lib/wayland. Don't use -static: the panel must load the shared
# GTK/glib/cairo libraries at runtime.
#
# pkg-config resolves gtk+-3.0 (headers/libs) provided by gtk+3.0-dev in the
# build container (Alpine 3.20), which matches the runtime in gtk/.

CC      := musl-gcc
PKGCONF ?= pkg-config

PKGS    := gtk+-3.0
PKGS_WL := wayland-client
CFLAGS  += -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -O2 -fPIC \
	$(shell $(PKGCONF) --cflags $(PKGS))
LDLIBS  += $(shell $(PKGCONF) --libs $(PKGS)) \
	$(shell $(PKGCONF) --libs $(PKGS_WL))

OUT     := ../../build/bin/kyronix-panel
ROOTFS  := ../../rootfs

SRCS := main.c panel.c clock.c menu.c style.c de.c taskbar.c icons.c
OBJS := $(SRCS:.c=.o)

.PHONY: all install clean check-musl check-pkgconf

all: check-musl check-pkgconf $(OUT)

check-musl:
	@command -v $(CC) >/dev/null 2>&1 || { echo "musl-gcc not found"; exit 1; }

check-pkgconf:
	@$(PKGCONF) --exists $(PKGS) || { \
		echo "panel: missing gtk+3.0 dev (install gtk+3.0-dev)"; exit 1; }
	@$(PKGCONF) --exists $(PKGS_WL) || { \
		echo "panel: missing wayland-client dev"; exit 1; }

$(OUT): $(OBJS)
	@mkdir -p $(dir $(OUT))
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: all
	@mkdir -p $(ROOTFS)/usr/bin
	cp $(OUT) $(ROOTFS)/usr/bin/kyronix-panel

clean:
	rm -f $(OBJS) $(OUT) $(ROOTFS)/usr/bin/kyronix-panel
