ROOTFS := ../rootfs

.PHONY: all clean

all:
	$(MAKE) -C init_
	$(MAKE) -C shell
	$(MAKE) -C sbase -f Makefile.kyronix
	$(MAKE) -C fetch
	mkdir -p $(ROOTFS)/bin
	cp ../build/bin/kyronix-init $(ROOTFS)/bin/init
	cp ../build/bin/ksh $(ROOTFS)/bin/
	for f in ../build/bin/*; do \
	    case "$$(basename $$f)" in \
	        kyronix-init|ksh|sbase-tools.stamp) ;; \
	        *) cp $$f $(ROOTFS)/bin/ ;; \
	    esac \
	done

clean:
	$(MAKE) -C init_ clean
	$(MAKE) -C shell clean
	-$(MAKE) -C sbase -f Makefile.kyronix clean 2>/dev/null
	$(MAKE) -C fetch clean
	rm -rf $(ROOTFS)/bin
