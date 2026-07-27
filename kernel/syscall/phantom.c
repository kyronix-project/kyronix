#include "phantom.h"

#include "internal.h"
#include "lib/string.h"
#include "security/phantom.h"

int64_t sys_phantom_mode(int mode) {
    if (!host_priv()) return -(int64_t) EPERM;
    if (mode < 0 || (uint32_t) mode > PHANTOM_TRAP) return -(int64_t) EINVAL;
    phantom_set_mode((uint32_t) mode);
    return (int64_t) phantom_get_mode();
}

int64_t sys_phantom_read(void *out, uint32_t max_events) {
    if (!out || !max_events || max_events > PHANTOM_EVENT_RING)
        return -(int64_t) EINVAL;
    if (!host_priv()) return -(int64_t) EPERM;
    uint64_t bytes = (uint64_t) max_events * sizeof(phantom_event_t);
    if (!uptr_ok_w(out, bytes)) return -(int64_t) EFAULT;
    phantom_event_t events[PHANTOM_EVENT_RING];
    uint32_t n = phantom_read(events, max_events);
    memcpy(out, events, (uint64_t) n * sizeof(phantom_event_t));
    return (int64_t) n;
}
