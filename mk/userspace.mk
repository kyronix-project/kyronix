USERSPACE_STAMP := $(BUILD)/.userspace

USERSPACE_SOURCES := $(shell find user -type f \
	\( -name '*.c' -o -name '*.h' -o -name 'Makefile' -o -name '*.md' \) \
	-not -path '*/autom4te.cache/*' | sort)

$(USERSPACE_STAMP): $(USERSPACE_SOURCES) $(BUILD)/libatomic_asneeded.a
	$(MAKE) -C user
	@mkdir -p $(@D)
	@touch $@
