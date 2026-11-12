#include "rseq.h"

#include "internal.h"
#include "lib/log.h"
#include "syscall.h"

int64_t sys_rseq(uint64_t rseq_ptr, uint64_t rseq_len, int flags, uint32_t sig) {
    if (rseq_len != RSEQ_SIZE || flags != 0) return -(int64_t) EINVAL;

    if (!uptr_ok_w((const void *) rseq_ptr, RSEQ_SIZE)) return -(int64_t) EFAULT;

    volatile uint64_t *cs = (volatile uint64_t *) (uintptr_t) (rseq_ptr + 8);
    *cs = 0;

    (void) sig;
    return 0;
}
