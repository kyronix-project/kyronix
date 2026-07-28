ALPINE_REPO := https://dl-cdn.alpinelinux.org/alpine/v3.20
OUT         := ../../build/bin
ROOTFS      := ../../rootfs
TMPDIR      := /tmp/kyronix-extras

APK_VERSIONS := \
	gzip-1.13-r0 \
	bzip2-1.0.8-r6 \
	xz-5.8.3-r0 \
	tar-1.35-r2 \
	libacl-2.3.2-r0 \
	libattr-2.5.2-r0

APK_FILES := $(addsuffix .apk,$(APK_VERSIONS))
STANDALONE := bzip2 xz gzip tar
ALIASES := gunzip bunzip2 unxz

.PHONY: all clean install

all: $(STANDALONE:%=$(OUT)/%) $(ALIASES:%=$(OUT)/%)

$(OUT)/bzip2 $(OUT)/xz: $(OUT)/.extras-fetch
	@rm -f $@
	@cp $(TMPDIR)/extracted/usr/bin/$(notdir $@) $@
	@chmod +x $@
	@echo "  extras: $(notdir $@)"

$(OUT)/gzip $(OUT)/tar: $(OUT)/.extras-fetch
	@rm -f $@
	@cp $(TMPDIR)/extracted/bin/$(notdir $@) $@
	@chmod +x $@
	@echo "  extras: $(notdir $@)"

$(OUT)/gunzip: $(OUT)/gzip
	@ln -sf gzip $@

$(OUT)/bunzip2: $(OUT)/bzip2
	@ln -sf bzip2 $@

$(OUT)/unxz: $(OUT)/xz
	@ln -sf xz $@

$(OUT)/.extras-fetch: $(APK_FILES:%=$(TMPDIR)/%)
	@mkdir -p $(TMPDIR)/extracted
	@for apk in $(APK_FILES:%=$(TMPDIR)/%); do \
		echo "  extras: extracting $$apk"; \
		tar -xzf $$apk -C $(TMPDIR)/extracted; \
	done
	@touch $@

$(TMPDIR)/%.apk:
	@mkdir -p $(TMPDIR)
	@curl -sL -o $@ $(ALPINE_REPO)/main/x86_64/$*.apk
	@if file $@ 2>/dev/null | grep -q HTML; then rm -f $@; touch $@; fi

install: all
	@mkdir -p $(ROOTFS)/bin $(ROOTFS)/lib
	@rm -f $(ROOTFS)/bin/busybox $(ROOTFS)/bin/install \
		$(ROOTFS)/bin/md5sum $(ROOTFS)/bin/sha256sum
	@for bin in $(STANDALONE); do \
		rm -f $(ROOTFS)/bin/$$bin; \
		cp $(OUT)/$$bin $(ROOTFS)/bin/; \
	done
	@for alias in $(ALIASES); do \
		rm -f $(ROOTFS)/bin/$$alias; \
	done
	@ln -sf gzip $(ROOTFS)/bin/gunzip
	@ln -sf bzip2 $(ROOTFS)/bin/bunzip2
	@ln -sf xz $(ROOTFS)/bin/unxz
	@cp -a $(TMPDIR)/extracted/lib/libacl.so* $(ROOTFS)/lib/
	@cp -a $(TMPDIR)/extracted/lib/libattr.so* $(ROOTFS)/lib/
	@echo "  extras: standalone tools installed (no BusyBox)"

clean:
	rm -f $(STANDALONE:%=$(OUT)/%) $(ALIASES:%=$(OUT)/%)
	rm -f $(OUT)/busybox $(OUT)/md5sum $(OUT)/sha256sum $(OUT)/install
	rm -f $(ROOTFS)/lib/libacl.so* $(ROOTFS)/lib/libattr.so*
