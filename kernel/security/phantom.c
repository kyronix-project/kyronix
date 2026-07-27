#include "security/phantom.h"

#include "lib/log.h"
#include "lib/string.h"
#include "proc/proc.h"

extern volatile uint64_t g_ticks;
static volatile uint32_t g_mode;
static volatile uint64_t g_sequence;
static volatile uint32_t g_write;
static volatile uint32_t g_pending;
static phantom_event_t g_events[PHANTOM_EVENT_RING];

void phantom_init(void) {
    memset(g_events, 0, sizeof(g_events));
#ifdef CONFIG_PHANTOM_FORKING
    g_mode = PHANTOM_AUDIT;
#else
    g_mode = PHANTOM_OFF;
#endif
    g_sequence = 0;
    g_write = 0;
    g_pending = 0;
    log_info("PHANTOM: live trap hooks ready (disabled)");
}

void phantom_set_mode(uint32_t mode) {
    if (mode > PHANTOM_TRAP) mode = PHANTOM_OFF;
    g_mode = mode;
    log_info("PHANTOM: mode=%u", mode);
}

uint32_t phantom_get_mode(void) { return g_mode; }

void phantom_record(uint32_t kind, uint32_t flags, uint64_t address,
                    uint64_t instruction, const char *detail) {
    if (g_mode == PHANTOM_OFF) return;
    uint32_t slot = __atomic_fetch_add(&g_write, 1, __ATOMIC_RELAXED) % PHANTOM_EVENT_RING;
    phantom_event_t *event = &g_events[slot];
    event->sequence = __atomic_add_fetch(&g_sequence, 1, __ATOMIC_RELAXED);
    event->tick = g_ticks;
    event->pid = g_current_proc ? g_current_proc->pid : 0;
    event->jail_id = g_current_proc ? g_current_proc->jail_id : 0;
    event->kind = kind;
    event->flags = flags;
    event->address = address;
    event->instruction = instruction;
    if (detail) strncpy(event->detail, detail, PHANTOM_DETAIL_MAX - 1);
    else event->detail[0] = '\0';
    event->detail[PHANTOM_DETAIL_MAX - 1] = '\0';
    __atomic_store_n(&g_pending, 1, __ATOMIC_RELEASE);
}

bool phantom_fault_candidate(const cpu_state_t *state, uint64_t address) {
    if (g_mode == PHANTOM_OFF || !state || !g_current_proc) return false;
    bool present = (state->error_code & 1u) != 0;
    bool write = (state->error_code & 2u) != 0;
    bool execute = (state->error_code & 16u) != 0;
    if (!present || (!write && !execute)) return false;
    phantom_record(PHANTOM_EVENT_FAULT, (uint32_t) state->error_code, address,
                   state->rip, execute ? "user instruction violation" : "user write violation");
    return g_mode == PHANTOM_TRAP;
}

bool phantom_pending(void) { return __atomic_load_n(&g_pending, __ATOMIC_ACQUIRE) != 0; }

void phantom_safe_point(void) {
    if (!phantom_pending()) return;
    __atomic_store_n(&g_pending, 0, __ATOMIC_RELEASE);
    /* Future policy workers consume this outside exception context. */
}

uint32_t phantom_read(phantom_event_t *out, uint32_t max_events) {
    if (!out || !max_events) return 0;
    uint32_t n = max_events < PHANTOM_EVENT_RING ? max_events : PHANTOM_EVENT_RING;
    uint32_t end = __atomic_load_n(&g_write, __ATOMIC_ACQUIRE);
    uint32_t available = end < PHANTOM_EVENT_RING ? end : PHANTOM_EVENT_RING;
    if (n > available) n = available;
    for (uint32_t i = 0; i < n; i++) out[i] = g_events[(end - n + i) % PHANTOM_EVENT_RING];
    return n;
}
