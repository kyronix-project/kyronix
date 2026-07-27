#include "jitter.h"

#include "internal.h"
#include "lib/string.h"
#include "security/anti_toctou.h"

int64_t sys_anti_toctou(uint32_t action, void *out) {
    if (!host_priv()) return -(int64_t) EPERM;
    switch (action) {
    case ANTI_TOCTOU_CTL_QUERY:
        return anti_toctou_enabled() ? 1 : 0;
    case ANTI_TOCTOU_CTL_ENABLE:
        anti_toctou_set_enabled(true);
        return anti_toctou_enabled() ? 1 : 0;
    case ANTI_TOCTOU_CTL_DISABLE:
        anti_toctou_set_enabled(false);
        return 0;
    case ANTI_TOCTOU_CTL_RESET:
        anti_toctou_reset();
        return anti_toctou_enabled() ? 1 : 0;
    case ANTI_TOCTOU_CTL_STATS: {
        if (!out || !uptr_ok_w(out, sizeof(anti_toctou_stats_t)))
            return -(int64_t) EFAULT;
        anti_toctou_stats_t stats;
        anti_toctou_get_stats(&stats);
        memcpy(out, &stats, sizeof(stats));
        return 0;
    }
    default:
        return -(int64_t) EINVAL;
    }
}
