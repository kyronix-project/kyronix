# Kyronix build system shim
# Usage:
#   make all                       — normal build
#   make all CRUNTIME=podman       — build inside container
#   make iso CRUNTIME=podman       — build ISO inside container
#   make clean CRUNTIME=podman     — clean inside container

CONTAINER_IMAGE   ?= kyronix-build
CONTAINER_RUNTIME ?= podman
CONTAINER_IMAGE_TAG ?= latest

# Rebuild image only when Containerfile changes
_CONTAINERFILE_TS := $(shell stat -c %Y Containerfile 2>/dev/null || echo 0)
_IMAGE_EXISTS     := $(shell $(CONTAINER_RUNTIME) image exists $(CONTAINER_IMAGE):$(CONTAINER_IMAGE_TAG) && echo 1 || echo 0)

ifdef CRUNTIME
  ifeq ($(origin INSIDE_CONTAINER),undefined)
    _GOALS := $(or $(MAKECMDGOALS),all)

    .PHONY: _build_image _do_container

    _build_image:
	@if [ "$(_IMAGE_EXISTS)" = "0" ] || \
	    [ "$(_CONTAINERFILE_TS)" -gt "$(shell $(CONTAINER_RUNTIME) inspect --format '{{.Created}}' $(CONTAINER_IMAGE):$(CONTAINER_IMAGE_TAG) 2>/dev/null | xargs -I{} date -d {} +%s 2>/dev/null || echo 0)" ]; then \
		echo "  Rebuilding container image..."; \
		$(CONTAINER_RUNTIME) build --layers -t $(CONTAINER_IMAGE):$(CONTAINER_IMAGE_TAG) -f Containerfile .; \
	else \
		echo "  Using cached container image"; \
	fi

    _do_container: _build_image
	@$(CONTAINER_RUNTIME) run --rm -e TERM -v $(CURDIR):/src:Z -w /src \
		$(CONTAINER_IMAGE):$(CONTAINER_IMAGE_TAG) bash -c "\
		rm -f limine/limine && \
		make -j$$(nproc) INSIDE_CONTAINER=1 $(_GOALS)"

    %: _do_container
	@true

  else
    include Makefile.build
  endif
else
  include Makefile.build
endif
