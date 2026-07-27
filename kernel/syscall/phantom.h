#pragma once

#include <stdint.h>

#define SYS_phantom_mode 507
#define SYS_phantom_read 508
#define SYS_phantom_clone 509
#define SYS_phantom_control 510

int64_t sys_phantom_mode(int mode);
int64_t sys_phantom_read(void *out, uint32_t max_events);
int64_t sys_phantom_control(uint32_t action, uint32_t pid);
