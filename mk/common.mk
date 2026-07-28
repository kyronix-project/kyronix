ifneq (, $(shell command -v x86_64-elf-gcc 2>/dev/null))
CC := x86_64-elf-gcc
LD := x86_64-elf-ld
else
CC := gcc
LD := ld
endif

HOSTCC ?= cc
LIMINE := limine
LWIP   := kernel/net/lwip/src

$(BUILD)/libatomic_asneeded.a:
	@mkdir -p $(@D)
	ar rcs $@
