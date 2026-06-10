ROOTFS := ../rootfs

.PHONY: all clean

all:
	$(MAKE) -C init
	$(MAKE) -C shell
	$(MAKE) -C login
	$(MAKE) -C vi
	$(MAKE) -C sbase -f Makefile.kyronix
	$(MAKE) -C fetch
	$(MAKE) -C tcc -f Makefile.kyronix
	$(MAKE) -C nasm -f Makefile.kyronix
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
	$(MAKE) -C init clean
	$(MAKE) -C shell clean
	$(MAKE) -C login clean
	$(MAKE) -C vi clean
	-$(MAKE) -C sbase -f Makefile.kyronix clean 2>/dev/null
	$(MAKE) -C fetch clean
	-$(MAKE) -C tcc -f Makefile.kyronix clean 2>/dev/null
	-$(MAKE) -C nasm -f Makefile.kyronix clean 2>/dev/null
	rm -rf $(ROOTFS)/bin
