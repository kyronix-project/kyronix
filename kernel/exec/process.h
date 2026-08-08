#pragma once
#include "exec/elf.h"
#include "mm/vmm.h"
#include <stdint.h>

#define USER_STACK_PAGES 4
#define USER_STACK_TOP 0x7fffffff0000ULL
#define USER_STACK_BASE (USER_STACK_TOP - (uint64_t) USER_STACK_PAGES * PAGE_SIZE)
#define USER_STACK_GROW_BASE 0x7ffff0000000ULL
#define USER_STACK_GROW_SLOP_PAGES 32

uint64_t kern_rand64(void);

int process_exec(const void *data, uint64_t size, const char *name);

uint64_t setup_user_stack(vmm_space_t *space, const elf_load_result_t *elf, int argc,
                          const char *const *argv, const char *const *envp,
                          uint32_t uid, uint32_t euid, uint32_t gid, uint32_t egid,
                          bool secure_exec);

__attribute__((noreturn)) void enter_userspace(uint64_t rip, uint64_t rsp, uint64_t rflags);

__attribute__((noreturn)) void enter_userspace_exec(uint64_t rip, uint64_t rsp, uint64_t rflags);
