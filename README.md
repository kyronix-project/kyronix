# Kyronix

> Operating system that sucks less.

Kyronix is a hobby x86-64 operating system focused on POSIX/Linux compatibility.

## Features

* Limine bootloader
* x86-64 kernel
* Ring 3 userspace
* ELF64 loader
* Syscall interface
* Virtual memory manager
* VFS + initramfs (CPIO)
* Pipes and signals
* Framebuffer + TTY
* Process management
* POSIX-style userspace

## Userspace

Includes:

* ksh
* vi
* login
* Kyrobox

## Build

```sh
make clean && make all && make run
```

## Project Structure

```text
kernel/      Kernel source
user/        Userspace programs
rootfs/      Root filesystem
iso_root/    Bootable ISO tree
ROADMAP.md   Development roadmap
```

## Status

Work in progress. Currently capable of booting into a Unix-like userspace environment and running statically linked applications.
