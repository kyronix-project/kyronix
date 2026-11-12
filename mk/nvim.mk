NVIM_APPIMG := $(BUILD)/nvim.appimage
NVIM_STAMP := $(BUILD)/nvim-stage/.stamp

NVIM_SCRIPT_CMD := python3 $(CURDIR)/scripts/fetch_nvim_appimage.py '$(abspath $(NVIM_APPIMG))'

ifeq ($(strip $(INSIDE_CONTAINER)),1)
NVIM_BUILD := sh -c "$(NVIM_SCRIPT_CMD)"
else
NVIM_BUILD := $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
    sh -c "python3 /src/scripts/fetch_nvim_appimage.py '/src/$(NVIM_APPIMG)'"
endif

# A real AppImage is needed to prove the whole path end to end; the synthetic
# fixtures are built by us and cannot show that a released image works. Gated
# on a stamp so a build without network does not re-download on every run.
$(NVIM_STAMP): scripts/fetch_nvim_appimage.py
	@$(call phase,neovim AppImage)
	@mkdir -p $(@D)
	@out=$$($(NVIM_BUILD) 2>&1); \
	if echo "$$out" | grep -q STAGE_OK && [ -s $(NVIM_APPIMG) ]; then \
	    touch $@; \
	    $(call ok,$(NVIM_APPIMG)); \
	else \
	    rm -f $(NVIM_APPIMG); \
	    echo "$$out" | tail -5; \
	    touch $@; \
	    $(call warn,neovim AppImage unavailable (offline?) - real-app tests will skip); \
	fi
