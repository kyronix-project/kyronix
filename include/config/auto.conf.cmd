autoconfig := include/config/auto.conf

deps_config := \
	kernel/Kconfig \

$(autoconfig): $(deps_config)
$(deps_config): ;
