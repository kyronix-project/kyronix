KERNEL_CONFIG := kernel/config.h
INSTRUMENT ?= 0
KERNEL_INSTRUMENT_CFLAGS = \
	$(if $(filter 1,$(INSTRUMENT)),-fno-omit-frame-pointer,-fomit-frame-pointer)

CFLAGS := \
	-std=c11 -O2 -DNDEBUG \
	-Wall -Wextra -Wno-unused-parameter -Wno-error \
	-ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	-m64 -march=x86-64 -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
	-mno-red-zone -mcmodel=kernel -U_FORTIFY_SOURCE \
	-include $(KERNEL_CONFIG) \
	-Ikernel -Ikernel/boot -Ikernel/net -Ikernel/mm \
	-I$(LWIP)/include -DNO_LOG

LDFLAGS := -T linker.ld -nostdlib -static -z max-page-size=0x1000

KERNEL_C_SRCS := \
	kernel/kernel.c \
	kernel/arch/x86_64/gdt.c \
	kernel/arch/x86_64/idt.c \
	kernel/arch/x86_64/lapic.c \
	kernel/arch/x86_64/pit.c \
	kernel/mm/pmm.c \
	kernel/mm/vmm.c \
	kernel/mm/vma.c \
	kernel/security/phantom.c \
	kernel/security/anti_toctou.c \
	kernel/mm/heap.c \
	kernel/mm/shm.c \
	kernel/mm/shmem.c \
	kernel/arch/x86_64/syscall_setup.c \
	kernel/syscall/syscall.c \
	kernel/syscall/mem.c \
	kernel/syscall/ptrace.c \
	kernel/syscall/futex.c \
	kernel/syscall/jailsys.c \
	kernel/syscall/cred.c \
	kernel/syscall/fsops.c \
	kernel/syscall/sig.c \
	kernel/syscall/procctl.c \
	kernel/syscall/epoll.c \
	kernel/syscall/file.c \
	kernel/syscall/poll.c \
	kernel/syscall/socket.c \
	kernel/syscall/time.c \
	kernel/syscall/mount.c \
	kernel/syscall/phantom.c \
	kernel/syscall/jitter.c \
	kernel/exec/elf.c \
	kernel/exec/process.c \
	kernel/module/loader.c \
	kernel/prof/profiler.c \
	kernel/proc/proc.c \
	kernel/proc/jail.c \
	kernel/proc/signal.c \
	kernel/proc/smp.c \
	kernel/fs/ext2.c \
	kernel/fs/vfs.c \
	kernel/fs/devfs.c \
	kernel/fs/eventfd.c \
	kernel/fs/signalfd.c \
	kernel/fs/fdctl.c \
	kernel/fs/fdpipe.c \
	kernel/fs/procfs.c \
	kernel/fs/unix_socket.c \
	kernel/fs/inet_socket.c \
	kernel/fs/pipe.c \
	kernel/fs/cpio.c \
	kernel/fs/fat32.c \
	kernel/fs/fstab.c \
	kernel/fs/partition.c \
	kernel/drivers/block/block.c \
	kernel/drivers/block/blockdev.c \
	kernel/drivers/ata/ahci.c \
	kernel/drivers/char/serial.c \
	kernel/drivers/input/kbd.c \
	kernel/drivers/tty/tty.c \
	kernel/drivers/video/fb.c \
	kernel/drivers/bus/pci/pci.c \
	kernel/drivers/acpi/acpi.c \
	kernel/drivers/bus/i2c/i2c.c \
	kernel/drivers/bus/spi/spi.c \
	kernel/drivers/hwmon/tmp117.c \
	kernel/net/net.c \
	kernel/net/lwip_glue.c \
	kernel/net/netif/kyronix_netif.c \
	$(LWIP)/core/init.c \
	$(LWIP)/core/def.c \
	$(LWIP)/core/dns.c \
	$(LWIP)/core/inet_chksum.c \
	$(LWIP)/core/ip.c \
	$(LWIP)/core/mem.c \
	$(LWIP)/core/memp.c \
	$(LWIP)/core/netif.c \
	$(LWIP)/core/pbuf.c \
	$(LWIP)/core/raw.c \
	$(LWIP)/core/stats.c \
	$(LWIP)/core/sys.c \
	$(LWIP)/core/tcp.c \
	$(LWIP)/core/tcp_in.c \
	$(LWIP)/core/tcp_out.c \
	$(LWIP)/core/timeouts.c \
	$(LWIP)/core/udp.c \
	$(LWIP)/core/ipv4/etharp.c \
	$(LWIP)/core/ipv4/icmp.c \
	$(LWIP)/core/ipv4/ip4.c \
	$(LWIP)/core/ipv4/ip4_addr.c \
	$(LWIP)/core/ipv4/ip4_frag.c \
	$(LWIP)/netif/ethernet.c \
	kernel/drivers/char/uio.c \
	kernel/drivers/video/fbdev.c \
	kernel/drivers/input/input.c \
	kernel/drivers/input/ps2mouse.c \
	kernel/drivers/tty/vt.c \
	kernel/lib/string.c \
	kernel/lib/printf.c \
	kernel/lib/log.c \
	kernel/lib/kallsyms.c \
	kernel/mm/kmemleak.c \
	kernel/crypto/chacha20.c \
	kernel/mm/llfree.c \
	kernel/mm/lower.c \
	kernel/mm/bitfield.c \
	kernel/mm/trees.c \
	kernel/mm/tree.c \
	kernel/mm/local.c \
	kernel/mm/child.c

KERNEL_ASM_SRCS := \
	kernel/arch/x86_64/idt_stubs.S \
	kernel/arch/x86_64/syscall_entry.S \
	kernel/proc/sched.S \
	kernel/proc/ap_trampoline.S \
	kernel/drivers/tty/psf_font.S

KERNEL_OBJS := \
	$(KERNEL_C_SRCS:%.c=$(BUILD)/%.o) \
	$(KERNEL_ASM_SRCS:%.S=$(BUILD)/%.o)
KERNEL_DEPS := $(KERNEL_OBJS:.o=.d)
VIRTIO_NET_MODULE := $(BUILD)/modules/virtio_net.ko
KERNEL_MODULES := $(VIRTIO_NET_MODULE)
MODULE_DEPS := $(KERNEL_MODULES:.ko=.d)
KALLSYMS_SRC := $(BUILD)/kallsyms_data.c
KALLSYMS_OBJ := $(BUILD)/kernel/kallsyms_data.o
KALLSYMS_SEED_OBJ := $(BUILD)/kernel/lib/kallsyms_seed.o
KERNEL_PRE := $(BUILD)/kernel.pre.elf

$(KERNEL): $(KERNEL_OBJS) $(KALLSYMS_OBJ) linker.ld $(KERNEL_CONFIG)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS) $(KALLSYMS_OBJ)

$(KERNEL_PRE): $(KERNEL_OBJS) $(KALLSYMS_SEED_OBJ) linker.ld $(KERNEL_CONFIG)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJS) $(KALLSYMS_SEED_OBJ)

$(BUILD)/%.o: %.c $(KERNEL_CONFIG)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(KERNEL_INSTRUMENT_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) -m64 -march=x86-64 -c $< -o $@

$(VIRTIO_NET_MODULE): kernel/drivers/net/virtio_net.c kernel/module.h $(KERNEL_CONFIG)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(KERNEL_INSTRUMENT_CFLAGS) -fno-asynchronous-unwind-tables -MMD -MP -c $< -o $@

$(KALLSYMS_SRC): $(KERNEL_PRE) mk/kernel.mk
	@echo "  GEN     $@"
	@mkdir -p $(@D)
	@nm -n -g --defined-only $(KERNEL_PRE) | \
	awk 'BEGIN { n = 0; \
	             print "#include <stdint.h>"; \
	             print "typedef struct { uint64_t addr; const char *name; } sym_entry_t;"; } \
	     $$2 ~ /^[TtWwRrDdBb]$$/ && $$3 ~ /^[A-Za-z_][A-Za-z0-9_]*$$/ && \
	             $$3 != "kallsyms_table" && $$3 != "kallsyms_num" { \
	             printf "extern char %s[];\n", $$3; names[n] = $$3; n++ } \
	     END { print "const sym_entry_t kallsyms_table[] = {"; \
	           for (i = 0; i < n; i++) \
	               printf "    { (uint64_t)(uintptr_t)&%s[0], \"%s\" },\n", names[i], names[i]; \
	           print "};"; print "const int kallsyms_num = " n ";" }' > $@.tmp
	mv $@.tmp $@

$(KALLSYMS_OBJ): $(KALLSYMS_SRC) $(KERNEL_CONFIG)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(KERNEL_INSTRUMENT_CFLAGS) -MMD -MP -c $(KALLSYMS_SRC) -o $@

.PHONY: _kallsyms
_kallsyms: $(KALLSYMS_SRC)

-include $(KERNEL_DEPS) $(MODULE_DEPS)
