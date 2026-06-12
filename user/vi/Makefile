CC := musl-gcc
CFLAGS := -std=c11 -O2 -march=x86-64 -mtune=generic -fno-pie -L../../build -D_GNU_SOURCE
WARN := -Wall -Wno-parentheses -Wno-dangling-pointer -Wno-address -Wno-unused-but-set-variable -Wno-stringop-truncation -Werror
LDFLAGS := -static -no-pie -L../../build
STAGE := ../../build/bin

.PHONY: all clean

all: vi
	mkdir -p $(STAGE)
	cp vi $(STAGE)/vi

vi: vi.c stubs.c libbb.h
	$(CC) $(CFLAGS) $(WARN) vi.c stubs.c -o vi $(LDFLAGS)

clean:
	rm -f vi $(STAGE)/vi
