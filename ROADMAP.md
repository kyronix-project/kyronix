# Kyronix — roadmap

Target: Linux-compatible x86-64 OS capable of running real userspace without patches.

`[x]` = done · `[~]` = partial · `[ ]` = not started

Drafted: June 2026. Horizon: 2 years (until June 2028).

---

## Implemented

### Kernel
- [x] GDT / IDT / TSS / PIC 8259 remap
- [x] PMM (bitmap + free-stack), VMM (4-level page tables, NX, HHDM), heap
- [x] Demand paging — #PF allocates page on the fly
- [x] PIT ~1000 Hz → g_ticks → preemptive scheduler (IRQ0)
- [x] SYSCALL/SYSRET + swapgs, TSS.rsp0, SSE/FPU
- [x] RTC → g_epoch_base (real Unix time)

### Processes and signals
- [x] ELF64 loader + PIE (ET_DYN) + shebang (#!)
- [x] PT_INTERP → dynamic linker (musl) + AT_BASE in auxv
- [x] fork / execve / wait4 / exit / exit_group
- [x] Cooperative + preemptive scheduler (sched_switch in asm)
- [x] Signals: rt_sigaction, rt_sigreturn, kill, SIGCHLD, SIGPIPE
- [x] clone() with CLONE_VM (basic threads)
- [x] nanosleep with real blocking (wakeup_tick + IRQ0 wake)
- [x] futex FUTEX_WAIT / FUTEX_WAKE (real blocking)

### VFS and filesystem
- [x] VFS ramfs: O_CREAT, O_TRUNC, write, unlink, mkdir, rename, chmod, chown
- [x] CPIO initrd, symlinks, chr-dev
- [x] pipe / pipe2 / dup / dup2 / dup3
- [x] /dev/tty, null, zero, urandom, stdin/stdout/stderr
- [x] /proc/version, /proc/self/exe, /proc/self/fd/N
- [x] Per-process cwd; chdir, getcwd, *at-syscalls (mkdirat, unlinkat etc.)

### Memory
- [x] mmap (anonymous + file-backed MAP_PRIVATE), mprotect, munmap, mremap, brk
- [x] PROT_EXEC correctly removes NX bit
- [x] mmap PROT_EXEC for dynamic linker

### Syscalls & compatibility
- [x] 150+ syscalls (read/write/open/close, stat, poll, select, epoll, sendfile etc.)
- [x] clock_gettime / gettimeofday (real time from RTC + g_ticks)
- [x] getrandom (RDRAND + TSC fallback)
- [x] arch_prctl FS/GS base (TLS)

### Userspace
- [x] musl libc as dynamic linker (/lib/ld-musl-x86_64.so.1)
- [x] /bin/sh → ksh, /usr/bin/env → /bin/env
- [x] ksh: pipes, redirects, history, tilde expansion
- [x] Kyrobox: standalone binaries for basic POSIX commands
- [x] Kyrobox: find/sed/sort/uniq/tr/dd/du/cksum and basic text filters
- [ ] Kyrobox: awk/tar/md5sum/sha*sum and extended flags
- [x] tcc 0.9.28 (C compiler), NASM (assembler)

---

## Quarter 1 — June–August 2026
### Interactive shell (what you see every day)

**Job control in ksh**
- [ ] `&` — background execution, shell continues
- [ ] Ctrl+C → SIGINT to foreground process group
- [ ] Ctrl+Z → SIGTSTP, `fg` / `bg` commands
- [ ] TTY: deliver signals via tty to foreground pgid

**Tab completion in ksh**
- [ ] Tab → complete file names and commands from PATH
- [ ] Double-Tab → show candidates list

**poll/select real blocking**
- [ ] Currently busy-wait — eats CPU and breaks program event loops
- [ ] `fd_wait_queue_t` per fd, process → PROC_WAITING until event
- [ ] Wake up on read/write-ready (pipe, tty, chr-dev)

**TTY discipline**
- [ ] ISIG: Ctrl+C = SIGINT, Ctrl+Z = SIGTSTP, Ctrl+\ = SIGQUIT
- [ ] IXON: Ctrl+S / Ctrl+Q (flow control)
- [ ] canonical mode (line-based) vs raw mode (byte-based)

---

## Quarter 2 — September–November 2026
### Persistent storage

**PCI enumeration**
- [ ] Walk PCI Configuration Space (ports 0xCF8/0xCFC)
- [ ] Find devices by vendor/device ID
- [ ] Basic device table, BAR mapping

**virtio-blk driver**
- [ ] Found 1af4:1001 → initialize virtqueue
- [ ] Read/write sectors (request + response descriptor ring)
- [ ] Integrate with VFS as block device

**ext2 filesystem**
- [ ] Parse superblock, group descriptors, inodes
- [ ] Read files and directories (read-only for now)
- [ ] Write: block allocation, file/directory creation
- [ ] fsck-compatible format (verify magic 0xEF53)

**VFS: mount table**
- [ ] `vfs_mount(path, fstype, dev)` / `vfs_umount(path)`
- [ ] Support multiple mount points simultaneously
- [ ] Read /etc/fstab at boot (tmpfs / ext2)

---

## Quarter 3 — December 2026–February 2027
### Networking: L2/L3

**virtio-net driver**
- [ ] PCI device 1af4:1000 → virtqueue RX/TX
- [ ] Send and receive ethernet frames
- [ ] Interrupt-driven + polling fallback

**Network stack — L2/L3**
- [ ] ARP: reply to requests, neighbor cache
- [ ] IPv4: fragmentation, checksum, routing table (1 route)
- [ ] ICMP: echo request/reply (ping works)
- [ ] DHCP client (simple: DISCOVER/OFFER/REQUEST/ACK over UDP)

**UDP sockets**
- [ ] `socket(AF_INET, SOCK_DGRAM)` / `bind` / `sendto` / `recvfrom`
- [ ] Port multiplexer, RX/TX buffers
- [ ] DNS resolver: gethostbyname over UDP :53

---

## Quarter 4 — March–May 2027
### Networking: TCP + socket API

**TCP stack**
- [ ] State machine: CLOSED → SYN_SENT → ESTABLISHED → FIN_WAIT → TIME_WAIT
- [ ] Sliding window, ACK, retransmit timeout (RTO)
- [ ] Passive open: listen/accept

**BSD sockets**
- [ ] `socket` / `bind` / `listen` / `accept` / `connect`
- [ ] `send` / `recv` / `sendmsg` / `recvmsg`
- [ ] `setsockopt` / `getsockopt` (SO_REUSEADDR, TCP_NODELAY)
- [ ] AF_UNIX (unix domain sockets — needed for IPC)
- [ ] select/poll/epoll on sockets

**Utilities**
- [ ] ping (ICMP), wget/curl (HTTP GET over TCP)
- [ ] nc (netcat), simple HTTP server

---

## Quarter 5 — June–August 2027
### Threads (pthreads)

**clone() full implementation**
- [ ] CLONE_VM + CLONE_FS + CLONE_FILES + CLONE_SIGHAND + CLONE_THREAD
- [ ] Allocate user-stack for thread (if child_stack != 0)
- [ ] Shared fd-table, vmm_space, cwd between threads of same process

**TLS (Thread-Local Storage)**
- [ ] Each thread has its own fs_base (arch_prctl SET_FS)
- [ ] Copy TLS template from PT_TLS segment of ELF
- [ ] __thread variables work correctly

**Synchronization**
- [ ] futex FUTEX_REQUEUE / FUTEX_CMP_REQUEUE
- [ ] Correct exit_thread (not exit_group) for set_tid_address
- [ ] robust futex (FUTEX_WAIT_REQUEUE_PI, rough implementation)

**Test**
- [ ] Program with multiple pthread_create runs
- [ ] pthread_mutex_lock/unlock work via futex

---

## Quarter 6 — September–November 2027
### APIC and SMP

**Local APIC**
- [ ] Detect via CPUID + ACPI MADT
- [ ] Enable xAPIC / x2APIC
- [ ] APIC timer as PIT replacement (TSC-deadline mode if available)
- [ ] Spurious interrupt vector

**I/O APIC**
- [ ] Parse MADT, IOAPIC base address
- [ ] Route legacy IRQs → vectors
- [ ] Keyboard, serial, timer via IOAPIC

**SMP: AP startup**
- [ ] SIPI sequence (startup IPI → AP enters protected/long mode)
- [ ] Per-CPU GDT/IDT/TSS/stack for each core
- [ ] Spinlock (test-and-set, lock prefix)
- [ ] Per-CPU current process pointer

**SMP: scheduler**
- [ ] Run queue per CPU (or simple global queue with spinlock)
- [ ] IPI for preemption between cores
- [ ] Correct idle exit via hlt with sti

---

## Quarter 7 — December 2027–February 2028
### Security and access rights

**VFS permissions**
- [ ] Check mode bits (rwxrwxrwx) on open/create/exec
- [ ] uid/gid checked properly (not hardcoded to 0)
- [ ] sticky bit for /tmp

**Users**
- [ ] Parse /etc/passwd and /etc/shadow
- [ ] /etc/group
- [ ] login with password check (crypt/sha-512)
- [ ] setuid/setgid binaries (sudo, su)

**chroot and namespaces**
- [ ] `chroot(path)` — changes VFS root for process
- [ ] Basic isolation: CLONE_NEWNS (mount namespace)
- [ ] PID namespace (for containers — stretch goal)

**Capabilities**
- [ ] Basic set: CAP_CHOWN, CAP_KILL, CAP_NET_BIND_SERVICE etc.
- [ ] Check capabilities instead of "root only"

---

## Quarter 8 — March–June 2028
### PTY, advanced I/O, polish

**PTY (pseudo-terminal)**
- [ ] /dev/ptmx — open master
- [ ] /dev/pts/N — slave sides (devpts pseudo-FS)
- [ ] Master↔slave protocol (TIOCGPTN, TIOCSPTLCK)
- [ ] Needed for: SSH, tmux, screen, any terminal multiplexer

**Advanced I/O**
- [ ] inotify (IN_CREATE, IN_MODIFY, IN_DELETE)
- [ ] timerfd / eventfd / signalfd
- [ ] io_uring (submit queue + completion queue) — stretch goal
- [ ] splice / tee (zero-copy between fds)

**Graphics (stretch goal)**
- [ ] virtio-gpu Wayland compositor (framebuffer mode)
- [ ] Minimal DRM/KMS interface
- [ ] Simple window manager on framebuffer

**Utilities and compatibility**
- [ ] ssh (client — uses TCP + PTY)
- [ ] Python 3 interpreter (statically linked with musl)
- [ ] Lua / MicroPython as embedded language
- [ ] pkg: simple package manager (tar.gz + manifest)
- [ ] Full /proc/meminfo, /proc/cpuinfo, /proc/net/...

---

## Long-term goals (beyond 2028)

- [ ] NVMe driver (PCIe, not virtio)
- [ ] USB stack (xHCI → HID keyboard/mouse)
- [ ] Bluetooth (HCI over USB)
- [ ] ACPI: S3 suspend-to-RAM, battery status
- [ ] btrfs or F2FS (copy-on-write FS)
- [ ] Full dynamic linking (dlopen/dlsym/dlclose)
- [ ] VDSO (clock_gettime without syscall via shared page)
- [ ] seccomp-BPF (syscall filtering)
- [ ] cgroups v2 (resource limits)
- [ ] Full container runtime (like podman)
- [ ] RISC-V 64 support (second arch target)

---

## Summary by year

| Period | Focus | Key result |
|--------|-------|-----------|
| Q1 2026 | Shell UX | job control, tab completion, real poll |
| Q2 2026 | Storage | virtio-blk + ext2 + mount table |
| Q3 2026 | Network L2/L3 | ping, DHCP, UDP, DNS |
| Q4 2026 | TCP + sockets | wget works, AF_UNIX |
| Q5 2027 | pthreads | multithreaded programs run |
| Q6 2027 | SMP | 2+ cores, APIC timer |
| Q7 2027 | Security | users, permissions, chroot |
| Q8 2028 | PTY + polish | tmux, SSH, full compatibility |

---

## Dependency order

```
Job control → TTY signals → poll blocking
virtio-blk → ext2 → mount table → persistent /home
virtio-net → ARP/IP → UDP/DNS → TCP → BSD sockets → HTTP
clone threads → TLS → futex improvements → pthreads
APIC → TSC → SMP → per-CPU sched
VFS permissions → users → chroot → namespaces → capabilities
PTY → SSH → tmux
```
