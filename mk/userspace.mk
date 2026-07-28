USERSPACE_STAMP := $(BUILD)/.userspace

USERSPACE_SOURCES := $(shell find user -type f \
	-not -path '*/autom4te.cache/*' \
	-not -name '*.o' -not -name 'config.log' \
	-not -name 'config.status' -not -name 'stamp-h1' | sort)

$(USERSPACE_STAMP): $(USERSPACE_SOURCES) $(BUILD)/libatomic_asneeded.a
	$(MAKE) -C user
	@mkdir -p $(@D)
	@touch $@
