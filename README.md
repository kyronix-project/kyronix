# Kyronix

<img align="left" src="meta/logo.png" width="120" alt="Kyronix logo">

Operating system that sucks less.

![Commits per month](https://img.shields.io/github/commit-activity/m/kyronix-project/kyronix)
[![test](https://github.com/kyronix-project/kyronix/actions/workflows/test.yml/badge.svg)](https://github.com/kyronix-project/kyronix/actions/workflows/test.yml)
[![ISC](https://img.shields.io/badge/license-ISC-blue)](#)
[![x86-64](https://img.shields.io/badge/arch-x86__64-lightgrey)](#)
[![AI](https://img.shields.io/badge/AI--assisted-blueviolet)](#)

<br clear="left"/>

Kyronix is a modern hybrid operating system focused on
performance, security and stability.

<img src="meta/screenshots/preview.png" alt="Kyronix preview" width="640">

## Features

### Kernel
- x86-64, 4-level paging, SMEP, NX-bit
- Limine bootloader (BIOS + UEFI)
- Preemptive scheduler (~1000 Hz)
- ELF64 loader (PIE + musl)
- 150+ Linux-compatible syscalls
- Demand paging (`mmap`, `mprotect`, `mremap`, `brk`)
- RTC, CPUID, RDRAND

### Drivers
- Framebuffer console (PSF fonts)
- PS/2 keyboard & mouse
- PCI enumeration
- AHCI (SATA)
- virtio-net
- Serial console (COM1)
- evdev (`/dev/input/event*`)
- Virtual terminals
- UIO

### Filesystems
- POSIX VFS
- CPIO initramfs
- Ext2 (R/W)
- FAT32 (R/W)
- procfs, devfs
- eventfd, pipe, AF_UNIX sockets

### Networking ([LwIP](https://github.com/stm32duino/lwip))
- ARP, IPv4, ICMP, UDP, TCP
- DHCP client
- AF_INET socket API
- ping, wget, nc

### Userspace
- **ksh** shell
- **vi** text editor
- **kyrobox** POSIX utilities
- **login** authentication
- **pkg** package manager
- Runs musl-linked applications

### Advanced
- POSIX signals
- `clone()` threads
- Jails (FS/PID/IPC isolation)
- Shared memory
- futex
- epoll, poll, select
- Continuous integration test suite

### Experimental security hooks

Kyronix contains an opt-in `PHANTOM_FORKING` telemetry layer. In audit mode it
records present-page user write/execute violations, denied VFS access, and TCP
connect outcomes in a bounded lock-free event ring. Trap mode scores suspicious
events per process and defers cloning to the next syscall safe point, so an
exception handler never allocates or forks directly.

The deferred clone shares user pages through copy-on-write and enters a private
FS/PID/IPC/privilege jail with a synthetic VFS root, dummy cryptographic
material, and simulated AF_INET acknowledgements. A new process remains in the
internal `PROC_EMBRYO` state until all isolation and descriptor setup has
completed. Publication to the scheduler is atomic; a setup failure rolls back
the unpublished process and records a fail-closed telemetry event.

The rootfs self-test is `/bin/phantom-test`. Run it as root after boot; it
enables audit mode, generates controlled VFS/network events, and prints the
captured ring entries. It also invokes the root-only Phantom COW clone syscall
and prints the resulting child PID/jail event. It exits non-zero if the ring
cannot be read or remains empty. In trap mode, a controlled VFS denial also
checks the deferred automatic clone at the next syscall safe point. The test
also exhausts the jail table temporarily to verify that a failed setup cannot
run an unconfined child.

Before a Phantom child is scheduled, inherited descriptors are sanitized:
host files, pipes, eventfds, timerfds, and Unix sockets above stderr are
closed, while AF_INET descriptors are replaced with independent simulated
endpoints. Socket lifetime is reference-counted so child teardown cannot close
the parent's live connection. Phantom jail slots and temporary VFS roots are
retired when the sandbox exits.

Trap mode uses a per-process score instead of cloning on every error. A lookup
denial contributes 10 points, denied writes 30, denied execution 50, and a
protection fault 100. The threshold is 100 points within five seconds, followed
by a ten-second cooldown and a budget of four automatic clones per process.

Mode 3 adds source-timeline quarantine. After the sandbox is committed, the
original process is parked in `PROC_QUARANTINED` and cannot return to user mode.
A host-privileged controller uses syscall 510 to query the sandbox PID and
explicitly resume or terminate the parked source. The wake-up path waits until
the source has left every per-CPU `current` slot before publishing its kernel
stack back to the run queue.

Present-page user write and execute protection faults are intercepted in mode
3. Exception context copies the complete interrupt frame into a bounded static
queue, takes a process reference, and parks the source without allocating
memory. A dedicated `[phantom-worker]` kernel thread builds the unpublished COW
child, gives only that child a private page with the required write or execute
permission, commits the jail/descriptor overlays, and publishes it. The child
returns through `iretq` to retry the exact faulting instruction. The original
timeline remains quarantined. Because retrying it would immediately repeat the
same fault, controller `RESUME` is rejected for fault quarantines; the
controller must terminate that source after collecting the sandbox telemetry.

This remains an experimental research mechanism, not production exploit
containment. The fault queue has eight fixed slots and fails closed when
saturated or when worker-side sandbox construction fails. Non-present page
faults, kernel faults, and unsupported exceptions still follow the normal
signal/panic path. There is also no checkpoint rollback for safely restoring a
fault-quarantined source timeline.

## Build

### Dependencies

```sh
gcc musl-tools qemu-system xorriso nasm
```

### Quick start

```sh
make clean && make all && make run
```

Without graphics:

```sh
make clean && make all && make run-serial
```

### Make targets

| Target | Description |
|---------|-------------|
| `all` | Build everything |
| `iso` | Build ISO image |
| `run` | Launch in QEMU |
| `run-serial` | Launch with serial console |
| `test-run` | Run tests |
| `test-run-log` | Run tests with logging |
| `user-build` | Build userspace |
| `fmt` | Format source |
| `fmt-check` | Check formatting |
| `clean` | Remove build artifacts |

## Project structure

| Directory | Purpose |
|-----------|---------|
| `kernel/` | Kernel source |
| `user/` | Userspace |
| `rootfs/` | Initramfs |
| `limine/` | Bootloader |
| `meta/` | Assets & screenshots |

## Support

If you like Kyronix, consider supporting its development.

<a href="https://buymeacoffee.com/kyron1x">
  <img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" height="50" alt="Buy Me a Coffee">
</a>

## License

Distributed under the ISC License. See `LICENSE` for more information.
