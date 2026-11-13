SYSROOT_SQSH := $(BUILD)/debian-sysroot.sqsh
SYSROOT_STAGE := $(BUILD)/sysroot-stage
SYSROOT_STAMP := $(SYSROOT_STAGE)/.stamp
SYSROOT_DEBIAN := docker.io/library/debian:bookworm-slim

SYSROOT_SCRIPT_CMD := \
    set -e; export DEBIAN_FRONTEND=noninteractive; \
    apt-get update >/dev/null 2>&1; \
    apt-get install -y --no-install-recommends python3 ca-certificates xz-utils binutils tar >/dev/null 2>&1; \
    python3 /src/scripts/build_sysroot.py '/src/$(SYSROOT_STAGE)' && \
    sh /src/scripts/build_sysroot_caches.sh '/src/$(SYSROOT_STAGE)' && \
    echo STAGE_OK

ifeq ($(strip $(INSIDE_CONTAINER)),1)
SYSROOT_BUILD := @echo "STAGE_OK (prebuilt)"
SYSROOT_PACK := sh -c "command -v mksquashfs >/dev/null 2>&1 || apk add --no-cache squashfs-tools >/dev/null 2>&1; \
    mksquashfs $(SYSROOT_STAGE) $(SYSROOT_SQSH) -comp xz -b 131072 -noappend -no-progress"
else
SYSROOT_BUILD := $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    $(SYSROOT_DEBIAN) sh -c "$(SYSROOT_SCRIPT_CMD)"
SYSROOT_PACK := $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
    sh -c "command -v mksquashfs >/dev/null 2>&1 || apk add --no-cache squashfs-tools >/dev/null 2>&1; \
        mksquashfs /src/$(SYSROOT_STAGE) /src/$(SYSROOT_SQSH) -comp xz -b 131072 -noappend -no-progress"
endif

$(SYSROOT_STAMP): scripts/build_sysroot.py scripts/build_sysroot_caches.sh
ifeq ($(strip $(INSIDE_CONTAINER)),1)
	@if [ -f $(SYSROOT_STAMP) ]; then \
	    $(call ok,sysroot staged (prebuilt)); \
	else \
	    $(call warn,sysroot stage missing - build it on the host first); \
	    false; \
	fi
else
	@$(call phase,glibc sysroot)
	@rm -rf $(SYSROOT_STAGE); mkdir -p $(SYSROOT_STAGE)
	@out=$$($(SYSROOT_BUILD) 2>/dev/null || true); \
	if echo "$$out" | grep -q STAGE_OK; then \
	    touch $(SYSROOT_STAMP); \
	    $(call ok,sysroot staged); \
	else \
	    rm -rf $(SYSROOT_STAGE); \
	    $(call warn,sysroot stage failed (offline?) - real-appimage tests will skip); \
	fi
endif

$(SYSROOT_SQSH): $(SYSROOT_STAMP)
	@if [ ! -f $(SYSROOT_STAMP) ] || [ ! -d $(SYSROOT_STAGE) ]; then \
	    $(call warn,no sysroot stage - skipping sysroot image); \
	else \
	    $(call phase,Pack sysroot); \
	    $(SYSROOT_PACK) >/dev/null; \
	    $(call ok,$(SYSROOT_SQSH)); \
	fi

REAL_APPIMG := $(BUILD)/real-app.img
REAL_APP_SQSH := $(BUILD)/real-app.sqsh
REAL_APP_STAGE := $(BUILD)/real-app-stage
APPRUN_SRC := user/testrunner/fixtures/appimg/AppRun.c

# Debian stage: compile AppRun and gather its runtime deps into the mounted
# staging tree at /src/build/real-app-stage (survives the container exit).
APPRUN_SCRIPT := \
    set -e; export DEBIAN_FRONTEND=noninteractive; \
    apt-get update >/dev/null 2>&1; \
    apt-get install -y --no-install-recommends gcc libc6-dev >/dev/null 2>&1; \
    rm -rf /src/$(REAL_APP_STAGE); mkdir -p /src/$(REAL_APP_STAGE)/libs; \
    gcc -O2 -o /src/$(REAL_APP_STAGE)/AppRun /src/$(APPRUN_SRC) -Wl,-rpath,'\$$ORIGIN/libs'; \
    chmod 755 /src/$(REAL_APP_STAGE)/AppRun; \
    cp -L /lib/x86_64-linux-gnu/libc.so.6 /src/$(REAL_APP_STAGE)/libs/; \
    cp -L /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 /src/$(REAL_APP_STAGE)/libs/; \
    test -s /src/$(REAL_APP_STAGE)/libs/libc.so.6; \
    test -s /src/$(REAL_APP_STAGE)/libs/ld-linux-x86-64.so.2; \
    echo APPRUN_OK

ifeq ($(strip $(INSIDE_CONTAINER)),1)
# Inside the Alpine container there is no glibc toolchain; the AppRun binary is
# produced by an outer host-side stage (see Makefile fixture-pre). Nothing to do.
APPRUN_BUILD := @echo "APPRUN_OK (prebuilt by outer stage)"
else
APPRUN_BUILD := $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    docker.io/library/debian:bookworm-slim sh -c "$(APPRUN_SCRIPT)"
endif

$(REAL_APP_STAGE): $(APPRUN_SRC)
	@$(call phase,Real AppImage fixture)
	rm -rf $(REAL_APP_STAGE); mkdir -p $(REAL_APP_STAGE)
	@out=$$($(APPRUN_BUILD) 2>/dev/null || true); \
	if echo "$$out" | grep -q APPRUN_OK && [ -f $(REAL_APP_STAGE)/AppRun ]; then \
	    $(call ok,AppRun staged); \
	else \
	    rm -rf $(REAL_APP_STAGE); \
	    $(call warn,AppRun build failed - real-appimage tests will skip); \
	fi

$(REAL_APP_SQSH): $(REAL_APP_STAGE) $(APPRUN_SRC)
ifeq ($(strip $(INSIDE_CONTAINER)),1)
	@command -v mksquashfs >/dev/null 2>&1 || apk add --no-cache squashfs-tools >/dev/null 2>&1; \
	    mksquashfs $(REAL_APP_STAGE) $(REAL_APP_SQSH) -comp xz -b 131072 -noappend -no-progress >/dev/null
else
	@$(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
	    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
	    sh -c "command -v mksquashfs >/dev/null 2>&1 || apk add --no-cache squashfs-tools >/dev/null 2>&1; \
	        mksquashfs /src/$(REAL_APP_STAGE) /src/$(REAL_APP_SQSH) -comp xz -b 131072 -noappend -no-progress" >/dev/null
endif

$(REAL_APPIMG): $(REAL_APP_SQSH) scripts/wrap_appimages.py
	@python3 $(CURDIR)/scripts/wrap_appimages.py --single $(REAL_APP_SQSH) $(REAL_APPIMG)
	@$(call ok,$(REAL_APPIMG))

WL_PROBEIMG := $(BUILD)/wl-probe.img
WL_PROBE_SQSH := $(BUILD)/wl-probe.sqsh
WL_PROBE_STAGE := $(BUILD)/wl-probe-stage
WL_PROBE_SRC := user/testrunner/fixtures/appimg/WaylandProbe.c

WL_PROBE_SCRIPT := \
    set -e; export DEBIAN_FRONTEND=noninteractive; \
    apt-get update >/dev/null 2>&1; \
    apt-get install -y --no-install-recommends gcc libc6-dev libwayland-dev >/dev/null 2>&1; \
    rm -rf /src/$(WL_PROBE_STAGE); mkdir -p /src/$(WL_PROBE_STAGE); \
    gcc -O2 -o /src/$(WL_PROBE_STAGE)/AppRun /src/$(WL_PROBE_SRC) -lwayland-client; \
    chmod 755 /src/$(WL_PROBE_STAGE)/AppRun; \
    echo WL_PROBE_OK

ifeq ($(strip $(INSIDE_CONTAINER)),1)
WL_PROBE_BUILD := @echo "WL_PROBE_OK (prebuilt by outer stage)"
else
WL_PROBE_BUILD := $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    docker.io/library/debian:bookworm-slim sh -c "$(WL_PROBE_SCRIPT)"
endif

$(WL_PROBE_STAGE): $(WL_PROBE_SRC)
	@$(call phase,Wayland client fixture)
	rm -rf $(WL_PROBE_STAGE); mkdir -p $(WL_PROBE_STAGE)
	@out=$$($(WL_PROBE_BUILD) 2>/dev/null || true); \
	if echo "$$out" | grep -q WL_PROBE_OK && [ -f $(WL_PROBE_STAGE)/AppRun ]; then \
	    $(call ok,Wayland probe staged); \
	else \
	    rm -rf $(WL_PROBE_STAGE); \
	    $(call warn,Wayland probe build failed - wayland tests will skip); \
	fi

$(WL_PROBE_SQSH): $(WL_PROBE_STAGE) $(WL_PROBE_SRC)
ifeq ($(strip $(INSIDE_CONTAINER)),1)
	@command -v mksquashfs >/dev/null 2>&1 || apk add --no-cache squashfs-tools >/dev/null 2>&1; \
	    mksquashfs $(WL_PROBE_STAGE) $(WL_PROBE_SQSH) -comp xz -b 131072 -noappend -no-progress >/dev/null
else
	@$(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
	    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
	    sh -c "command -v mksquashfs >/dev/null 2>&1 || apk add --no-cache squashfs-tools >/dev/null 2>&1; \
	        mksquashfs /src/$(WL_PROBE_STAGE) /src/$(WL_PROBE_SQSH) -comp xz -b 131072 -noappend -no-progress" >/dev/null
endif

$(WL_PROBEIMG): $(WL_PROBE_SQSH) scripts/wrap_appimages.py
	@python3 $(CURDIR)/scripts/wrap_appimages.py --single $(WL_PROBE_SQSH) $(WL_PROBEIMG)
	@$(call ok,$(WL_PROBEIMG))
