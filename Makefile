# Kyronix build entry point.
#
# Normal use:
#   make              build dist/kyronix.iso
#   make run          boot the ISO with a persistent install disk
#   make boot         boot the installed disk
#   make test         build and run the kernel test suite
#
# Add CRUNTIME=podman (or docker) to build inside Containerfile.

.DEFAULT_GOAL := iso

CONTAINER_RUNTIME ?= $(if $(strip $(CRUNTIME)),$(CRUNTIME),podman)
CONTAINER_IMAGE   ?= kyronix-build
CONTAINER_TAG     ?= latest

ifneq ($(strip $(CRUNTIME)),)
ifeq ($(strip $(INSIDE_CONTAINER)),)

include mk/output.mk

SERVE_GOALS := container-build container-run fixture-pre
REQUESTED_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),iso)
BUILD_GOALS := $(filter-out $(SERVE_GOALS),$(REQUESTED_GOALS))
CONTAINER_GOALS := $(filter-out run boot,$(BUILD_GOALS)) \
    $(if $(filter run,$(BUILD_GOALS)),iso) \
    $(if $(filter run boot,$(BUILD_GOALS)),dist/kyronix-disk.img)

.PHONY: container-build container-run fixture-pre $(REQUESTED_GOALS)

# Debian-toolchain fixtures (glibc objects, real-AppImage AppRun) must be built
# on the host via a throwaway debian container BEFORE the Alpine build runs,
# cuz the kyronix-build image has no glibc/apt. The inner make then treats
# these as already-present prerequisites and only packs/wraps them.
DEBIAN_FIXTURE_TARGETS := build/fixtures-glibc/.stamp build/real-app.img build/wl-probe.img build/sysroot-stage/.stamp

fixture-pre:
	@$(call phase,Debian fixtures (host))
	@$(MAKE) --no-print-directory CRUNTIME= INSIDE_CONTAINER= $(DEBIAN_FIXTURE_TARGETS)
	@$(call ok,debian fixtures ready)

container-build:
	@$(call phase,Building container image)
	$(CONTAINER_RUNTIME) build --layers \
	    -t $(CONTAINER_IMAGE):$(CONTAINER_TAG) -f Containerfile .
	@$(call ok,container image ready)

container-run: fixture-pre container-build
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
