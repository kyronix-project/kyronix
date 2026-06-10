#pragma once
#include "exec/elf.h"
#include "mm/vmm.h"
#include <stdint.h>

uint64_t kern_rand64(void);

int process_exec(const void* data, uint64_t size, const char* name);

uint64_t setup_user_stack(vmm_space_t* space, const elf_load_result_t* elf, int argc,
                          const char* const* argv, const char* const* envp);

__attribute__((noreturn)) void enter_userspace(uint64_t rip, uint64_t rsp, uint64_t rflags);

__attribute__((noreturn)) void enter_userspace_exec(uint64_t rip, uint64_t rsp, uint64_t rflags);
