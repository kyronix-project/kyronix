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

REQUESTED_GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),iso)

.PHONY: container-build container-run $(REQUESTED_GOALS)

container-build:
	$(CONTAINER_RUNTIME) build --layers \
	    -t $(CONTAINER_IMAGE):$(CONTAINER_TAG) -f Containerfile .

container-run: container-build
	$(CONTAINER_RUNTIME) run --rm \
	    -v $(CURDIR):/src:Z -w /src \
	    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
	    make -j$$(nproc) INSIDE_CONTAINER=1 $(REQUESTED_GOALS)

$(REQUESTED_GOALS): container-run
	@:

else
include mk/main.mk
endif
else
include mk/main.mk
endif
