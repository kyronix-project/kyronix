# Baked kair assets.
#
# The payloads live in assets/kair/ and are tracked by git, so `make clean`
# cannot destroy them. build/ only ever receives a copy. `make bake-*` is the
# only thing that writes assets/, and it runs on the host -- never inside the
# container (the Alpine build image has no glibc and no apt).
KAIR_ASSETS := assets/kair
KAIR_ASSET_REV ?= 1
KAIR_DEBIAN := docker.io/library/debian:bookworm-slim
KAIR_READMES := $(KAIR_ASSETS)/README.md $(KAIR_ASSETS)/SHA256SUMS

KAIR_GLIBC_ASSET := $(KAIR_ASSETS)/fixtures/glibc
KAIR_REAL_APP_ASSET := $(KAIR_ASSETS)/fixtures/real-app
KAIR_WL_PROBE_ASSET := $(KAIR_ASSETS)/fixtures/wl-probe
KAIR_BASH_ASSET := $(KAIR_ASSETS)/apps/bash
KAIR_NVIM_ASSET := $(KAIR_ASSETS)/apps/nvim.appimage
KAIR_GALCULATOR_ASSET := $(KAIR_ASSETS)/apps/galculator.appimage

KAIR_GLIBC_FILES := $(shell find $(KAIR_GLIBC_ASSET) -type f 2>/dev/null)
KAIR_REAL_APP_FILES := $(shell find $(KAIR_REAL_APP_ASSET) -type f 2>/dev/null)
KAIR_WL_PROBE_FILES := $(shell find $(KAIR_WL_PROBE_ASSET) -type f 2>/dev/null)

SYSROOT_SQSH := $(BUILD)/debian-runtime.sqsh
GLIBC_FIXTURES := $(BUILD)/fixtures-glibc
GLIBC_STAMP := $(GLIBC_FIXTURES)/.stamp
GLIBC_SRC := user/testrunner/fixtures/glibc/hello.c
REAL_APP_STAGE := $(BUILD)/real-app-stage
REAL_APP_SQSH := $(BUILD)/real-app.sqsh
REAL_APPIMG := $(BUILD)/real-app.img
APPRUN_SRC := user/testrunner/fixtures/appimg/AppRun.c
WL_PROBE_STAGE := $(BUILD)/wl-probe-stage
WL_PROBE_SQSH := $(BUILD)/wl-probe.sqsh
WL_PROBEIMG := $(BUILD)/wl-probe.img
WL_PROBE_SRC := user/testrunner/fixtures/appimg/WaylandProbe.c
BASH_STAGE := $(BUILD)/bash-stage
BASH_STAMP := $(BASH_STAGE)/.stamp
BASH_BIN := $(BASH_STAGE)/bin/bash
NVIM_APPIMG := $(BUILD)/nvim.appimage
NVIM_STAMP := $(BUILD)/nvim-stage/.stamp
GALCULATOR_APPIMG := $(BUILD)/galculator.appimage
GALCULATOR_STAMP := $(BUILD)/galculator-stage/.stamp

# squashfs-tools is in the Containerfile, so no apk fallback is needed there;
# a CRUNTIME= host build still has to borrow the image.
ifeq ($(strip $(INSIDE_CONTAINER)),1)
MKSQUASHFS = mksquashfs $(1) $(2) -comp xz -b 131072 -noappend -no-progress >/dev/null
else
MKSQUASHFS = $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
    sh -c "mksquashfs /src/$(1) /src/$(2) -comp xz -b 131072 -noappend -no-progress" >/dev/null
endif

# A container write leaves root-owned files on the bind mount when the runtime
# is rootful; the same guard `make clean` uses applies here.
KAIR_RM = if [ -e $(1) ]; then chmod -R u+w $(1) 2>/dev/null || true; rm -rf $(1); fi

MISSING_ASSET = $(call warn,missing baked asset $(1) - run 'make bake' (needs podman + network))

$(KAIR_GLIBC_ASSET) $(KAIR_REAL_APP_ASSET) \
$(KAIR_WL_PROBE_ASSET) $(KAIR_BASH_ASSET) $(KAIR_NVIM_ASSET) \
$(KAIR_GALCULATOR_ASSET):
	@$(call MISSING_ASSET,$@)
	@false

# --- consuming the baked assets -------------------------------------------

$(GLIBC_STAMP): $(KAIR_GLIBC_FILES)
	@$(call phase,Glibc fixtures)
	@$(call KAIR_RM,$(GLIBC_FIXTURES))
	mkdir -p $(GLIBC_FIXTURES)
	cp -a $(KAIR_GLIBC_ASSET)/. $(GLIBC_FIXTURES)/
	@touch $@
	@$(call ok,$(GLIBC_FIXTURES))

$(REAL_APP_STAGE)/.stamp: $(KAIR_REAL_APP_FILES)
	@$(call phase,Real AppImage fixture)
	@$(call KAIR_RM,$(REAL_APP_STAGE))
	mkdir -p $(REAL_APP_STAGE)
	cp -a $(KAIR_REAL_APP_ASSET)/. $(REAL_APP_STAGE)/
	@touch $@
	@$(call ok,$(REAL_APP_STAGE))

$(WL_PROBE_STAGE)/.stamp: $(KAIR_WL_PROBE_FILES)
	@$(call phase,Wayland client fixture)
	@$(call KAIR_RM,$(WL_PROBE_STAGE))
	mkdir -p $(WL_PROBE_STAGE)
	cp -a $(KAIR_WL_PROBE_ASSET)/. $(WL_PROBE_STAGE)/
	@touch $@
	@$(call ok,$(WL_PROBE_STAGE))

$(REAL_APP_SQSH): $(REAL_APP_STAGE)/.stamp
	@$(call phase,Pack real-app)
	$(call MKSQUASHFS,$(REAL_APP_STAGE),$(REAL_APP_SQSH))
	@$(call ok,$(REAL_APP_SQSH))

$(WL_PROBE_SQSH): $(WL_PROBE_STAGE)/.stamp
	@$(call phase,Pack wl-probe)
	$(call MKSQUASHFS,$(WL_PROBE_STAGE),$(WL_PROBE_SQSH))
	@$(call ok,$(WL_PROBE_SQSH))

$(REAL_APPIMG): $(REAL_APP_SQSH) scripts/wrap_appimages.py
	@python3 $(CURDIR)/scripts/wrap_appimages.py --single $(REAL_APP_SQSH) $(REAL_APPIMG) >/dev/null
	@$(call ok,$(REAL_APPIMG))

$(WL_PROBEIMG): $(WL_PROBE_SQSH) scripts/wrap_appimages.py
	@python3 $(CURDIR)/scripts/wrap_appimages.py --single $(WL_PROBE_SQSH) $(WL_PROBEIMG) >/dev/null
	@$(call ok,$(WL_PROBEIMG))

$(BASH_STAMP): $(KAIR_BASH_ASSET)
	@$(call phase,static bash)
	@$(call KAIR_RM,$(BASH_STAGE))
	mkdir -p $(BASH_STAGE)/bin $(BUILD)/bin
	cp -p $(KAIR_BASH_ASSET) $(BASH_BIN)
	cp -p $(KAIR_BASH_ASSET) $(BUILD)/bin/bash
	@touch $@
	@$(call ok,bash staged)

$(NVIM_STAMP): $(KAIR_NVIM_ASSET)
	@$(call phase,neovim AppImage)
	@mkdir -p $(@D)
	cp -p $(KAIR_NVIM_ASSET) $(NVIM_APPIMG)
	@touch $@
	@$(call ok,$(NVIM_APPIMG))

$(GALCULATOR_STAMP): $(KAIR_GALCULATOR_ASSET)
	@$(call phase,Galculator AppImage)
	@mkdir -p $(@D)
	cp -p $(KAIR_GALCULATOR_ASSET) $(GALCULATOR_APPIMG)
	@touch $@
	@$(call ok,$(GALCULATOR_APPIMG))

# --- baking ---------------------------------------------------------------
# Host-only. `bake-*` fills in whatever is missing, `rebake-*` forces. Neither
# is reached by a normal build.

.PHONY: bake bake-info bake-check rebake rebake-sysroot rebake-fixtures rebake-apps

SYSROOT_BAKE_STAGE := $(BUILD)/sysroot-bake
SYSROOT_BAKE_CMD := \
    set -e; export DEBIAN_FRONTEND=noninteractive; \
    apt-get update >/dev/null 2>&1; \
    apt-get install -y --no-install-recommends python3 ca-certificates xz-utils \
        binutils tar squashfs-tools >/dev/null 2>&1; \
    rm -rf /src/$(SYSROOT_BAKE_STAGE); mkdir -p /src/$(SYSROOT_BAKE_STAGE); \
    python3 /src/scripts/build_sysroot.py '/src/$(SYSROOT_BAKE_STAGE)' && \
    sh /src/scripts/build_sysroot_caches.sh '/src/$(SYSROOT_BAKE_STAGE)' && \
    mksquashfs /src/$(SYSROOT_BAKE_STAGE) /src/$(SYSROOT_SQSH) \
        -comp xz -b 131072 -noappend -no-progress >/dev/null && \
    rm -rf /src/$(SYSROOT_BAKE_STAGE) && \
    echo BAKE_OK

# Fixture sources are compiled in one Debian pass so the three apt-get updates
# collapse into one.
FIXTURE_BAKE_CMD := \
    set -e; export DEBIAN_FRONTEND=noninteractive; \
    apt-get update >/dev/null 2>&1; \
    apt-get install -y --no-install-recommends gcc libc6-dev libwayland-dev >/dev/null 2>&1; \
    rm -rf /src/$(KAIR_GLIBC_ASSET) /src/$(KAIR_REAL_APP_ASSET) /src/$(KAIR_WL_PROBE_ASSET); \
    mkdir -p /src/$(KAIR_GLIBC_ASSET) /src/$(KAIR_REAL_APP_ASSET)/libs /src/$(KAIR_WL_PROBE_ASSET); \
    gcc -O2 -o /src/$(KAIR_GLIBC_ASSET)/hello-glibc /src/$(GLIBC_SRC); \
    cp -L /lib64/ld-linux-x86-64.so.2 /usr/lib/x86_64-linux-gnu/libc.so.6 \
        /usr/lib/x86_64-linux-gnu/libm.so.6 /src/$(KAIR_GLIBC_ASSET)/; \
    gcc -O2 -o /src/$(KAIR_REAL_APP_ASSET)/AppRun /src/$(APPRUN_SRC) -Wl,-rpath,'\$$ORIGIN/libs'; \
    cp -L /lib/x86_64-linux-gnu/libc.so.6 /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 \
        /src/$(KAIR_REAL_APP_ASSET)/libs/; \
    gcc -O2 -o /src/$(KAIR_WL_PROBE_ASSET)/AppRun /src/$(WL_PROBE_SRC) -lwayland-client; \
    chmod 755 /src/$(KAIR_REAL_APP_ASSET)/AppRun /src/$(KAIR_WL_PROBE_ASSET)/AppRun; \
    test -s /src/$(KAIR_GLIBC_ASSET)/hello-glibc; \
    test -s /src/$(KAIR_REAL_APP_ASSET)/libs/libc.so.6; \
    test -s /src/$(KAIR_WL_PROBE_ASSET)/AppRun; \
    echo BAKE_OK

KAIR_HOST_ONLY = if [ "$(INSIDE_CONTAINER)" = "1" ]; then \
    $(call warn,bake must run on the host); false; \
fi

bake-sysroot:
	@$(call KAIR_HOST_ONLY)
	@if [ -s $(SYSROOT_SQSH) ]; then \
	    $(call ok,sysroot already baked); \
	else \
	    $(call phase,Baking glibc sysroot (rev $(KAIR_ASSET_REV))); \
	    mkdir -p $(BUILD); \
	    out=$$($(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z $(KAIR_DEBIAN) \
	        sh -c "$(SYSROOT_BAKE_CMD)" 2>&1); \
	    if echo "$$out" | grep -q BAKE_OK; then \
	        $(call ok,$(SYSROOT_SQSH)); \
	    else \
	        echo "$$out" | tail -10; \
	        $(call warn,sysroot bake failed - see output above); false; \
	    fi; \
	fi

bake-fixtures:
	@$(call KAIR_HOST_ONLY)
	@if [ -s $(KAIR_GLIBC_ASSET)/hello-glibc ] && [ -s $(KAIR_REAL_APP_ASSET)/AppRun ] \
	    && [ -s $(KAIR_WL_PROBE_ASSET)/AppRun ]; then \
	    $(call ok,fixtures already baked); \
	else \
	    $(call phase,Baking Debian fixtures (rev $(KAIR_ASSET_REV))); \
	    mkdir -p $(KAIR_ASSETS)/fixtures $(BUILD); \
	    out=$$($(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z $(KAIR_DEBIAN) \
	        sh -c "$(FIXTURE_BAKE_CMD)" 2>&1); \
	    if echo "$$out" | grep -q BAKE_OK; then \
	        $(call ok,fixtures); \
	    else \
	        echo "$$out" | tail -10; \
	        $(call warn,fixture bake failed - see output above); false; \
	    fi; \
	fi

bake-apps:
	@$(call KAIR_HOST_ONLY)
	@$(call phase,Baking apps (rev $(KAIR_ASSET_REV)))
	@mkdir -p $(KAIR_ASSETS)/apps $(BUILD)
	@mkdir -p $(BUILD)/bash-stage
	@out=$$($(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
	    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
	    sh -c "python3 /src/scripts/fetch_bash_static.py '/src/$(BASH_STAGE)'" 2>&1); \
	if echo "$$out" | grep -q STAGE_OK && [ -s $(BASH_BIN) ]; then \
	    cp -p $(BASH_BIN) $(KAIR_BASH_ASSET); \
	    $(call ok,$(KAIR_BASH_ASSET)); \
	else \
	    echo "$$out" | tail -5; \
	    $(call warn,static bash unavailable - apps not fully baked); false; \
	fi
	@out=$$($(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
	    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
	    sh -c "python3 /src/scripts/fetch_nvim_appimage.py '/src/$(KAIR_NVIM_ASSET)'" 2>&1); \
	if echo "$$out" | grep -q STAGE_OK && [ -s $(KAIR_NVIM_ASSET) ]; then \
	    $(call ok,$(KAIR_NVIM_ASSET)); \
	else \
	    echo "$$out" | tail -5; \
	    $(call warn,neovim AppImage unavailable (offline?) - real-app tests will skip); false; \
	fi
	@out=$$($(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
	    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
	    sh -c "python3 /src/scripts/fetch_galculator_appimage.py '/src/$(KAIR_GALCULATOR_ASSET)'" 2>&1); \
	if echo "$$out" | grep -q STAGE_OK && [ -s $(KAIR_GALCULATOR_ASSET) ]; then \
	    $(call ok,$(KAIR_GALCULATOR_ASSET)); \
	else \
	    echo "$$out" | tail -5; \
	    $(call warn,Galculator AppImage unavailable (offline?) - GUI test will skip); false; \
	fi

bake: bake-sysroot bake-fixtures bake-apps
	@$(call phase,Refreshing checksums)
	@cd $(KAIR_ASSETS) && find . -type f -not -name SHA256SUMS -not -name README.md \
	    | sort | xargs sha256sum > SHA256SUMS
	@$(call ok,assets/kair baked (rev $(KAIR_ASSET_REV)))

rebake-sysroot:
	@rm -f $(SYSROOT_SQSH)
	@$(MAKE) --no-print-directory bake-sysroot

rebake-fixtures:
	@rm -rf $(KAIR_GLIBC_ASSET) $(KAIR_REAL_APP_ASSET) $(KAIR_WL_PROBE_ASSET)
	@$(MAKE) --no-print-directory bake-fixtures

rebake-apps:
	@rm -f $(KAIR_BASH_ASSET) $(KAIR_NVIM_ASSET) $(KAIR_GALCULATOR_ASSET)
	@$(MAKE) --no-print-directory bake-apps

rebake:
	@rm -f $(SYSROOT_SQSH) $(KAIR_BASH_ASSET) $(KAIR_NVIM_ASSET) $(KAIR_GALCULATOR_ASSET)
	@rm -rf $(KAIR_GLIBC_ASSET) $(KAIR_REAL_APP_ASSET) $(KAIR_WL_PROBE_ASSET)
	@$(MAKE) --no-print-directory bake

bake-check:
	@$(call phase,Verifying baked assets)
	@cd $(KAIR_ASSETS) && sha256sum -c SHA256SUMS 2>&1 | grep -v ': OK$$' || true
	@cd $(KAIR_ASSETS) && sha256sum -c SHA256SUMS >/dev/null 2>&1 \
	    && $(call ok,all assets match SHA256SUMS) \
	    || { $(call fail,asset checksum mismatch); false; }

bake-info:
	@echo "assets:   $(KAIR_ASSETS)"
	@echo "revision: $(KAIR_ASSET_REV)"
	@echo "debian:   $(KAIR_DEBIAN)"
	@echo "runtime:  $(CONTAINER_RUNTIME) / $(CONTAINER_IMAGE):$(CONTAINER_TAG)"
	@du -sh $(KAIR_ASSETS) 2>/dev/null || true
