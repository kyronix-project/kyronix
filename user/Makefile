ROOTFS := ../rootfs

.PHONY: all clean

all:
	$(MAKE) -C shell
	$(MAKE) -C login
	$(MAKE) -C vi
	$(MAKE) -C sbase -f Makefile.kyronix
	$(MAKE) -C fetch
	mkdir -p $(ROOTFS)/bin
	cp -u ../build/bin/kyronix-init $(ROOTFS)/bin/init
	cp -u ../build/bin/ksh $(ROOTFS)/bin/
	for f in ../build/bin/*; do \
	    case "$$(basename $$f)" in \
	        kyronix-init|ksh|sbase-tools.stamp) ;; \
	        *) cp -u $$f $(ROOTFS)/bin/ ;; \
	    esac \
	done

clean:
	$(MAKE) -C shell clean
	$(MAKE) -C login clean
	$(MAKE) -C vi clean
	-$(MAKE) -C sbase -f Makefile.kyronix clean 2>/dev/null
	$(MAKE) -C fetch clean
	rm -rf $(ROOTFS)/bin
