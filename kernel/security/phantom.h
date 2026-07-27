#pragma once

#include "arch/x86_64/cpu.h"
#include <stdbool.h>
#include <stdint.h>

#define PHANTOM_OFF 0u
#define PHANTOM_AUDIT 1u
#define PHANTOM_TRAP 2u
#define PHANTOM_EVENT_FAULT 1u
#define PHANTOM_EVENT_ACCESS 2u
#define PHANTOM_EVENT_NETWORK 3u
#define PHANTOM_EVENT_CRYPTO 4u
#define PHANTOM_EVENT_RING 64u
#define PHANTOM_DETAIL_MAX 48u

typedef struct {
    uint64_t sequence, tick;
    uint32_t pid, jail_id, kind, flags;
    uint64_t address, instruction;
    char detail[PHANTOM_DETAIL_MAX];
} phantom_event_t;

void phantom_init(void);
void phantom_set_mode(uint32_t mode);
uint32_t phantom_get_mode(void);
void phantom_record(uint32_t kind, uint32_t flags, uint64_t address,
                    uint64_t instruction, const char *detail);
bool phantom_fault_candidate(const cpu_state_t *state, uint64_t address);
bool phantom_pending(void);
void phantom_safe_point(void);
uint32_t phantom_read(phantom_event_t *out, uint32_t max_events);
