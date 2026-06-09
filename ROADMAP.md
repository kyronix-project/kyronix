# Kyronix — roadmap

Target: x86-64 kernel that can run statically linked userspace binaries
via POSIX syscalls.

- [ ] = not started
- [x] = done

---

## Phase 1 — Segments & interrupts

- [x] GDT: null, kernel code/data (ring 0), user code/data (ring 3), TSS
- [ ] IDT: 256 vectors, load with `lidt`
- [ ] ISR dispatch: asm wrapper → `cpu_state_t` → C handler table
- [ ] PIC 8259: remap master (0x20) / slave (0x28), mask all
- [ ] Exception stubs: #PF, #GP, #DE, #SS — log + halt

## Phase 2 — Memory management

- [ ] PMM: bitmap + free-stack frame allocator
- [ ] VMM: map/unmap, page alloc, higher-half takeover from Limine
- [ ] Heap: `kmalloc`/`kfree` on top of page allocator

## Phase 3 — Timer

- [ ] PIT at 1000 Hz (or HPET)
- [ ] IRQ0 → tick counter → scheduler entry
- [ ] `sleep()` / `block_for()` primitives

## Phase 4 — Scheduler & kernel threads

- [ ] `struct thread` (regs, stack, state, page table pointer)
- [ ] Context switch (callee-saved regs, RSP, CR3)
- [ ] Round-robin `schedule()`, `yield()`
- [ ] Kernel thread creation

## Phase 5 — Userspace & syscall entry

- [ ] `iretq` to ring 3 (user SS/RSP/RFLAGS/CS/RIP on stack)
- [ ] User page tables (U/S bit)
- [ ] `syscall`/`sysret` MSRs (STAR, LSTAR, SF_MASK)
- [ ] Syscall dispatch table (RAX → handler)
- [ ] Shared header with syscall numbers

## Phase 6 — ELF loader

- [ ] ELF64 parser (magic, program headers, segment mapping)
- [ ] `exec`-like function: buffer → VMM mappings → ring 3 entry
- [ ] Test binary with `crt0` that issues a syscall

## Phase 7 — VFS & initramfs

- [ ] `struct inode`, `struct file`, `struct file_operations`
- [ ] Mount table
- [ ] initramfs: embedded tar/cpio → root mount
- [ ] Device nodes: serial, fb, null, zero

## Phase 8 — POSIX syscalls

- [ ] `exit`
- [ ] `write`
- [ ] `read`
- [ ] `open`
- [ ] `close`
- [ ] `brk` / `sbrk`
- [ ] `mmap` / `munmap`
- [ ] `getpid`
- [ ] `sched_yield`
- [ ] `nanosleep`

## Not in scope yet

- SMP / APIC
- PCI / AHCI / NVMe
- Networking
- Dynamic linking
- Full permission model / procfs