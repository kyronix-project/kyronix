BASH_STAGE := $(BUILD)/bash-stage
BASH_STAMP := $(BASH_STAGE)/.stamp
BASH_BIN := $(BASH_STAGE)/bin/bash

BASH_SCRIPT_CMD := python3 $(CURDIR)/scripts/fetch_bash_static.py '$(abspath $(BASH_STAGE))'

ifeq ($(strip $(INSIDE_CONTAINER)),1)
BASH_BUILD := sh -c "$(BASH_SCRIPT_CMD)"
else
BASH_BUILD := $(CONTAINER_RUNTIME) run --rm -v $(CURDIR):/src:Z \
    $(CONTAINER_IMAGE):$(CONTAINER_TAG) \
    sh -c "python3 /src/scripts/fetch_bash_static.py '/src/$(BASH_STAGE)'"
endif

# One target only: the stamp. A separate $(BUILD)/bin/bash rule let its recipe
# race the staging recipe under -j, each wiping the tree the other was filling.
# The offline path still touches the stamp, so a failed download does not make
# make re-drive the userspace build on every later invocation.
$(BASH_STAMP): scripts/fetch_bash_static.py
	@$(call phase,static bash)
	@rm -rf $(BASH_STAGE)
	@mkdir -p $(BUILD)/bin
	@out=$$($(BASH_BUILD) 2>&1); \
	if echo "$$out" | grep -q STAGE_OK && [ -s $(BASH_BIN) ]; then \
	    cp $(BASH_BIN) $(BUILD)/bin/bash; \
	    touch $(BASH_STAMP); \
	    $(call ok,bash staged); \
	else \
	    echo "$$out" | tail -5; \
	    rm -rf $(BASH_STAGE) $(BUILD)/bin/bash; \
	    mkdir -p $(BASH_STAGE); \
	    touch $(BASH_STAMP); \
	    $(call warn,bash stage failed (offline?) - script AppRuns will not run); \
	fi
