SYSROOT_SQSH := $(BUILD)/debian-sysroot.sqsh
SYSROOT_STAGE := $(BUILD)/sysroot-stage
SYSROOT_STAMP := $(SYSROOT_STAGE)/.stamp

# The staging script is self-contained (python3 stdlib + ar/tar/xz only).
SYSROOT_SCRIPT_CMD := python3 $(CURDIR)/scripts/build_sysroot.py '$(abspath $(SYSROOT_STAGE))'

ifeq ($(strip $(INSIDE_CONTAINER)),1)
SYSROOT_BUILD := sh -c "$(SYSROOT_SCRIPT_CMD)"
else
SYSROOT_BUILD := $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
    sh -c "python3 /src/scripts/build_sysroot.py '/src/$(SYSROOT_STAGE)'"
endif

$(SYSROOT_STAMP): scripts/build_sysroot.py
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

$(SYSROOT_SQSH): $(SYSROOT_STAMP)
	@$(call phase,Pack sysroot)
ifeq ($(strip $(INSIDE_CONTAINER)),1)
	@sh -c "command -v mksquashfs >/dev/null 2>&1 || apk add --no-cache squashfs-tools >/dev/null 2>&1; \
	    mksquashfs $(SYSROOT_STAGE) $(SYSROOT_SQSH) -comp xz -b 131072 -noappend -no-progress" >/dev/null
else
	@$(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
	    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
	    sh -c "command -v mksquashfs >/dev/null 2>&1 || apk add --no-cache squashfs-tools >/dev/null 2>&1; \
	        mksquashfs /src/$(SYSROOT_STAGE) /src/$(SYSROOT_SQSH) -comp xz -b 131072 -noappend -no-progress" >/dev/null
endif
	@$(call ok,$(SYSROOT_SQSH))

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

$(REAL_APPIMG): $(REAL_APP_SQSH)
	@python3 -c "d=bytearray(4096); d[0:4]=b'\x7fELF'; d[8:12]=b'AI\x02\x00'; \
	    open('$(REAL_APPIMG)','wb').write(d); \
	    open('$(REAL_APPIMG)','ab').write(open('$(REAL_APP_SQSH)','rb').read())"
	@$(call ok,$(REAL_APPIMG))
