GALCULATOR_APPIMG := $(BUILD)/galculator.appimage
GALCULATOR_STAMP := $(BUILD)/galculator-stage/.stamp

GALCULATOR_SCRIPT_CMD := python3 $(CURDIR)/scripts/fetch_galculator_appimage.py '$(abspath $(GALCULATOR_APPIMG))'

ifeq ($(strip $(INSIDE_CONTAINER)),1)
GALCULATOR_BUILD := sh -c "$(GALCULATOR_SCRIPT_CMD)"
else
GALCULATOR_BUILD := $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
    sh -c "python3 /src/scripts/fetch_galculator_appimage.py '/src/$(GALCULATOR_APPIMG)'"
endif

# Real thin AppImage (GTK3 app that relies on the glibc sysroot) used by the
# GUI smoke test. Gated on a stamp so offline builds do not re-download.
$(GALCULATOR_STAMP): scripts/fetch_galculator_appimage.py
	@$(call phase,Galculator AppImage)
	@mkdir -p $(@D)
	@out=$$($(GALCULATOR_BUILD) 2>&1); \
	if echo "$$out" | grep -q STAGE_OK && [ -s $(GALCULATOR_APPIMG) ]; then \
	    touch $@; \
	    $(call ok,$(GALCULATOR_APPIMG)); \
	else \
	    rm -f $(GALCULATOR_APPIMG); \
	    echo "$$out" | tail -5; \
	    touch $@; \
	    $(call warn,Galculator AppImage unavailable (offline?) - GUI test will skip); \
	fi
