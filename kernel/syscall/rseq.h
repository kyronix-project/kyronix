#pragma once

#include <stdint.h>

#define SYS_rseq 334
#define SYS_clone3 435
#define SYS_close_range 436

#define RSEQ_SIZE 32

int64_t sys_rseq(uint64_t rseq_ptr, uint64_t rseq_len, int flags, uint32_t sig);
