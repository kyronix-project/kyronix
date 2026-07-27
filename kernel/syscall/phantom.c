#include "phantom.h"

#include "internal.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/percpu.h"
#include "lib/string.h"
#include "proc/smp.h"
#include "security/phantom.h"

static bool phantom_proc_on_cpu(const proc_t *target) {
    uint32_t ncpu = g_cpu_count < MAX_CPUS ? g_cpu_count : MAX_CPUS;
    for (uint32_t i = 0; i < ncpu; i++)
        if (__atomic_load_n(&g_cpu_local[i].current, __ATOMIC_ACQUIRE) == target)
            return true;
    return false;
}

int64_t sys_phantom_mode(int mode) {
    if (!host_priv()) return -(int64_t) EPERM;
    if (mode < 0 || (uint32_t) mode > PHANTOM_QUARANTINE) return -(int64_t) EINVAL;
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

int64_t sys_phantom_control(uint32_t action, uint32_t pid) {
    if (!host_priv()) return -(int64_t) EPERM;
    if (action > PHANTOM_CTL_LAST_SANDBOX) return -(int64_t) EINVAL;

    proc_t *target = proc_find(pid);
    if (!target) return -(int64_t) ESRCH;

    uint32_t sandbox_pid;
    uint8_t quarantined;
    uint8_t fault_pending;
    spin_lock(&g_proctable_lock);
    sandbox_pid = target->phantom_sandbox_pid;
    quarantined = target->phantom_quarantined;
    fault_pending = target->phantom_fault_job_pending;

    if (action == PHANTOM_CTL_STATUS || action == PHANTOM_CTL_LAST_SANDBOX) {
        spin_unlock(&g_proctable_lock);
        proc_unref(target);
        if (action == PHANTOM_CTL_STATUS && !quarantined) return 0;
        return (int64_t) sandbox_pid;
    }

    if (!quarantined || target->state != PROC_QUARANTINED) {
        spin_unlock(&g_proctable_lock);
        proc_unref(target);
        return -(int64_t) EINVAL;
    }
    if (action == PHANTOM_CTL_RESUME && target->phantom_fault_quarantine) {
        spin_unlock(&g_proctable_lock);
        proc_unref(target);
        return -(int64_t) EPERM;
    }
    if (action == PHANTOM_CTL_TERMINATE && fault_pending) {
        target->phantom_quarantine_action = PHANTOM_CTL_TERMINATE;
        spin_unlock(&g_proctable_lock);
        phantom_record_for(target, PHANTOM_EVENT_QUARANTINE,
                           PHANTOM_QUARANTINEF_TERMINATE, pid, 0,
                           "fault-source termination queued");
        proc_unref(target);
        return 0;
    }

    target->phantom_quarantine_action = (uint8_t) action;
    target->phantom_quarantined = 0;
    spin_unlock(&g_proctable_lock);

    /*
     * The target marked itself quarantined before calling sched_switch().
     * Do not publish its kernel stack on another CPU until that switch has
     * removed it from every per-CPU current slot.
     */
    while (phantom_proc_on_cpu(target)) cpu_relax();

    spin_lock(&g_proctable_lock);
    if (target->state == PROC_QUARANTINED) {
        target->state = PROC_READY;
        proc_set_ready(target);
    }
    spin_unlock(&g_proctable_lock);

    phantom_record_for(target, PHANTOM_EVENT_QUARANTINE,
                       action == PHANTOM_CTL_TERMINATE
                           ? PHANTOM_QUARANTINEF_TERMINATE
                           : PHANTOM_QUARANTINEF_RESUME,
                       pid, sandbox_pid,
                       action == PHANTOM_CTL_TERMINATE
                           ? "source timeline terminated"
                           : "source timeline resumed");
    proc_unref(target);
    return 0;
}
