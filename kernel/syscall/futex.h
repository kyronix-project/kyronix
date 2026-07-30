#pragma once

#include <stdint.h>

// wake any futex waiters blocked on a cleared child-tid address (thread exit)
void cleartid_wake(uint32_t *addr);

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val, void *timeout, uint32_t *uaddr2,
                  uint32_t val3);
