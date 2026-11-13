# Kyronix build entry point.
#
# Normal use:
#   make              build dist/kyronix.iso
#   make run          boot the ISO with a persistent install disk
#   make boot         boot the installed disk
#   make test         build and run the kernel test suite
#   make bake         (re)bake the kair assets in assets/kair/ (host-only)
#
# Add CRUNTIME=podman (or docker) to build inside Containerfile.

.DEFAULT_GOAL := iso

CONTAINER_RUNTIME ?= $(if $(strip $(CRUNTIME)),$(CRUNTIME),podman)
CONTAINER_IMAGE   ?= kyronix-build
CONTAINER_TAG     ?= latest

ifneq ($(strip $(CRUNTIME)),)
ifeq ($(strip $(INSIDE_CONTAINER)),)

include mk/output.mk

HOST_GOALS := bake bake-sysroot bake-fixtures bake-apps \
    rebake rebake-sysroot rebake-fixtures rebake-apps bake-check bake-info
SERVE_GOALS := container-build container-run $(HOST_GOALS)
REQUESTED_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),iso)
BUILD_GOALS := $(filter-out $(SERVE_GOALS),$(REQUESTED_GOALS))
CONTAINER_GOALS := $(filter-out run boot,$(BUILD_GOALS)) \
    $(if $(filter run,$(BUILD_GOALS)),iso) \
    $(if $(filter run boot,$(BUILD_GOALS)),dist/kyronix-disk.img)

.PHONY: container-build container-run $(HOST_GOALS) $(REQUESTED_GOALS)

# Baking the kair assets needs a Debian toolchain and the network, neither of
# which the Alpine build image has, so it runs on the host. CRUNTIME= forces the
# host branch of this Makefile; CONTAINER_RUNTIME still defaults to podman for
# the throwaway debian container each bake uses.
$(HOST_GOALS):
	@$(MAKE) --no-print-directory CRUNTIME= INSIDE_CONTAINER= $@

container-build:
	@$(call phase,Building container image)
	$(CONTAINER_RUNTIME) build --layers \
	    -t $(CONTAINER_IMAGE):$(CONTAINER_TAG) -f Containerfile .
	@$(call ok,container image ready)

container-run: container-build
	@$(call phase,Running build inside container)
	$(CONTAINER_RUNTIME) run --rm \
	    -v $(CURDIR):/src:Z \
	    -v $(CURDIR)/./pkg-registry-src:/src/./pkg-registry-src:Z \
	    -w /src \
	    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
	    make -j$$(nproc) INSIDE_CONTAINER=1 $(CONTAINER_GOALS)

$(BUILD_GOALS): container-run
	@$(if $(filter run boot,$@),$(MAKE) INSIDE_CONTAINER=1 $@,:)

else
include mk/main.mk
endif
else
include mk/main.mk
endif
