#pragma once

#include <stdint.h>

#include "arch/x86_64/cpu.h"
#include "syscall.h"

struct proc;

int64_t sys_fork(syscall_frame_t *f);
int64_t sys_phantom_clone(syscall_frame_t *f);
int64_t proc_phantom_clone(syscall_frame_t *f);
int64_t proc_phantom_fault_clone(struct proc *source,
                                 const cpu_state_t *state, uint64_t address);
int64_t sys_clone(uint64_t flags, uint64_t child_stack, uint32_t *ptid, uint32_t *ctid,
                  uint64_t newtls, syscall_frame_t *f);
int64_t sys_execve(const char *path, const char **uargv, const char **uenvp);
int64_t sys_wait4(int pid, int *wstatus, int options, void *rusage);
int64_t sys_arch_prctl(int code, uint64_t addr);
