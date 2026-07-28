CC := musl-gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -march=x86-64 -mtune=generic -fno-pie -L../../build
LDFLAGS ?= -static -no-pie
OUT := ../../build/bin

FNO_LINK_LIBATOMIC := $(shell echo 'int main(void){return 0;}' | $(CC) -x c - -fno-link-libatomic -o /dev/null 2>/dev/null && echo '-fno-link-libatomic')
CFLAGS += $(FNO_LINK_LIBATOMIC)

SRCS := testrunner.c tests_fs.c tests_proc.c tests_signal.c tests_ipc.c tests_time.c tests_io.c tests_misc.c tests_modules.c tests_utils.c tests_jail.c tests_disk.c tests_smp.c

.PHONY: all clean

all: $(OUT)/testrunner

$(OUT)/testrunner: $(SRCS) test_harness.h Makefile
	mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(OUT)/testrunner
