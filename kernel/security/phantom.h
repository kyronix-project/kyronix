#pragma once

#include "arch/x86_64/cpu.h"
#include "syscall/syscall.h"
#include <stdbool.h>
#include <stdint.h>

#define PHANTOM_OFF 0u
#define PHANTOM_AUDIT 1u
#define PHANTOM_TRAP 2u
#define PHANTOM_QUARANTINE 3u
#define PHANTOM_EVENT_FAULT 1u
#define PHANTOM_EVENT_ACCESS 2u
#define PHANTOM_EVENT_NETWORK 3u
#define PHANTOM_EVENT_CRYPTO 4u
#define PHANTOM_EVENT_CLONE 5u
#define PHANTOM_EVENT_QUARANTINE 6u
#define PHANTOM_CLONEF_COW 0x01u
#define PHANTOM_CLONEF_FDS_SANITIZED 0x02u
#define PHANTOM_CLONEF_COMMITTED 0x04u
#define PHANTOM_CLONEF_FAILED 0x08u
#define PHANTOM_CLONEF_FAULT 0x10u
#define PHANTOM_CLONEF_WORKER 0x20u
#define PHANTOM_QUARANTINEF_HOLD 0x01u
#define PHANTOM_QUARANTINEF_RESUME 0x02u
#define PHANTOM_QUARANTINEF_TERMINATE 0x04u
#define PHANTOM_QUARANTINEF_READY 0x08u
#define PHANTOM_CTL_STATUS 0u
#define PHANTOM_CTL_RESUME 1u
#define PHANTOM_CTL_TERMINATE 2u
#define PHANTOM_CTL_LAST_SANDBOX 3u
#define PHANTOM_EVENT_RING 64u
#define PHANTOM_DETAIL_MAX 48u
#define PHANTOM_SCORE_THRESHOLD 100u
#define PHANTOM_SCORE_WINDOW_TICKS 5000u
#define PHANTOM_CLONE_COOLDOWN_TICKS 10000u
#define PHANTOM_CLONE_BUDGET 4u

typedef struct {
    uint64_t sequence, tick;
    uint32_t pid, jail_id, kind, flags;
    uint64_t address, instruction;
    char detail[PHANTOM_DETAIL_MAX];
} phantom_event_t;

struct proc;

void phantom_init(void);
void phantom_set_mode(uint32_t mode);
uint32_t phantom_get_mode(void);
uint32_t phantom_event_weight(uint32_t kind, uint32_t flags);
void phantom_record(uint32_t kind, uint32_t flags, uint64_t address,
                    uint64_t instruction, const char *detail);
void phantom_record_for(const struct proc *owner, uint32_t kind, uint32_t flags,
                        uint64_t address, uint64_t instruction,
                        const char *detail);
bool phantom_fault_candidate(const cpu_state_t *state, uint64_t address);
bool phantom_handle_fault(const cpu_state_t *state, uint64_t address);
bool phantom_worker_start(void);
bool phantom_pending(void);
void phantom_safe_point(syscall_frame_t *frame);
void phantom_quarantine_current(uint32_t sandbox_pid);
uint32_t phantom_read(phantom_event_t *out, uint32_t max_events);
