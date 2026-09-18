#include "net.h"
#include "arch/x86_64/spinlock.h"
#include "drivers/netdev.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"

#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

err_t kyronix_netif_init(struct netif *nif);
void kyronix_netif_input(struct netif *nif, const uint8_t *data, uint16_t len);

static struct netif g_netif;
static spinlock_t g_driver_lock;
static spinlock_t g_lifecycle_lock;
static const net_driver_ops_t *g_driver;
static netdev_t *g_local_device;
static uint32_t g_driver_calls;
static bool g_lwip_initialized;
static bool g_netif_active;
static bool g_starting;
static bool g_dhcp_active;
static proc_t *g_net_worker;
static volatile uint32_t g_poll_pending;

static const net_driver_ops_t *driver_acquire(bool require_netif, bool polling) {
    uint64_t flags = irq_save();
    spin_lock(&g_driver_lock);
    const net_driver_ops_t *ops = NULL;
    if (g_driver && (!require_netif || g_netif_active) && (!polling || !g_starting)) {
        ops = g_driver;
        g_driver_calls++;
    }
    spin_unlock(&g_driver_lock);
    irq_restore(flags);
    return ops;
}

static void driver_release(void) {
    uint64_t flags = irq_save();
    spin_lock(&g_driver_lock);
    g_driver_calls--;
    spin_unlock(&g_driver_lock);
    irq_restore(flags);
}

static void driver_drain(void) {
    for (;;) {
        uint64_t flags = irq_save();
        spin_lock(&g_driver_lock);
        bool idle = g_driver_calls == 0;
        spin_unlock(&g_driver_lock);
        irq_restore(flags);
        if (idle) return;
        cpu_relax();
    }
}

static void net_worker_main(void) {
    for (;;) {
        uint64_t flags = irq_save();
        if (!__atomic_exchange_n(&g_poll_pending, 0, __ATOMIC_ACQ_REL)) {
            g_current_proc->state = PROC_WAITING;
            if (__atomic_exchange_n(&g_poll_pending, 0, __ATOMIC_ACQ_REL)) {
                g_current_proc->state = PROC_RUNNING;
                __atomic_store_n(&g_poll_pending, 1, __ATOMIC_RELEASE);
                irq_restore(flags);
                continue;
            }
            irq_restore(flags);
            sched_block_current();
            continue;
        }
        irq_restore(flags);
        const net_driver_ops_t *ops = driver_acquire(true, true);
        if (!ops) continue;
        ops->poll();
        sys_check_timeouts();
        driver_release();
    }
}

static bool local_send(const uint8_t *data, uint16_t len) {
    netdev_t *nd = g_local_device;
    return nd && nd->send(nd, data, len) >= 0;
}

static void local_poll(void) {
    netdev_t *nd = g_local_device;
    if (nd) nd->poll(nd);
}

static const uint8_t *local_mac(void) {
    return g_local_device ? g_local_device->mac : NULL;
}

static const net_driver_ops_t g_local_ops = {
    .send = local_send,
    .poll = local_poll,
    .mac = local_mac,
};

static bool register_driver(const net_driver_ops_t *ops, netdev_t *nd) {
    if (!ops || !ops->send || !ops->poll || !ops->mac) return false;
    spin_lock(&g_lifecycle_lock);
    uint64_t flags = irq_save();
    spin_lock(&g_driver_lock);
    if (g_driver) {
        spin_unlock(&g_driver_lock);
        irq_restore(flags);
        spin_unlock(&g_lifecycle_lock);
        return false;
    }
    g_driver = ops;
    g_local_device = nd;
    g_starting = true;
    spin_unlock(&g_driver_lock);
    irq_restore(flags);

    if (!g_lwip_initialized) {
        lwip_init();
        dns_init();
        g_lwip_initialized = true;
    }
    if (!__atomic_load_n(&g_net_worker, __ATOMIC_ACQUIRE)) {
        proc_t *worker = proc_create_kernel("[net-rx]", net_worker_main);
        if (!worker) goto fail;
        __atomic_store_n(&g_net_worker, worker, __ATOMIC_RELEASE);
    }

    ip4_addr_t ip, mask, gw;
    if (nd) {
        ip4_addr_set_zero(&ip);
        ip4_addr_set_zero(&mask);
        ip4_addr_set_zero(&gw);
    } else {
        IP4_ADDR(&ip, 10, 0, 2, 15);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        IP4_ADDR(&gw, 10, 0, 2, 2);
    }
    if (!netif_add(&g_netif, &ip, &mask, &gw, NULL, kyronix_netif_init, ethernet_input))
        goto fail;

    flags = irq_save();
    spin_lock(&g_driver_lock);
    g_netif_active = true;
    spin_unlock(&g_driver_lock);
    irq_restore(flags);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    if (nd) {
        if (dhcp_start(&g_netif) != ERR_OK) {
            flags = irq_save();
            spin_lock(&g_driver_lock);
            g_netif_active = false;
            spin_unlock(&g_driver_lock);
            irq_restore(flags);
            driver_drain();
            dhcp_stop(&g_netif);
            dhcp_cleanup(&g_netif);
            netif_set_down(&g_netif);
            netif_remove(&g_netif);
            goto fail;
        }
        g_dhcp_active = true;
        log_info("net: DHCP started on %s", nd->name);
    } else {
        ip4_addr_t dns;
        IP4_ADDR(&dns, 10, 0, 2, 3);
        dns_setserver(0, &dns);
        log_info("net: static IP 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3");
    }
    flags = irq_save();
    spin_lock(&g_driver_lock);
    g_starting = false;
    spin_unlock(&g_driver_lock);
    irq_restore(flags);
    spin_unlock(&g_lifecycle_lock);
    net_schedule_poll();
    return true;

fail:
    flags = irq_save();
    spin_lock(&g_driver_lock);
    g_driver = NULL;
    g_netif_active = false;
    spin_unlock(&g_driver_lock);
    irq_restore(flags);
    driver_drain();
    g_local_device = NULL;
    g_starting = false;
    g_dhcp_active = false;
    spin_unlock(&g_lifecycle_lock);
    return false;
}

bool net_driver_register(const net_driver_ops_t *ops) {
    return register_driver(ops, NULL);
}

void net_driver_unregister(const net_driver_ops_t *ops) {
    if (!ops) return;
    spin_lock(&g_lifecycle_lock);
    uint64_t flags = irq_save();
    spin_lock(&g_driver_lock);
    if (g_driver != ops) {
        spin_unlock(&g_driver_lock);
        irq_restore(flags);
        spin_unlock(&g_lifecycle_lock);
        return;
    }
    g_netif_active = false;
    g_driver = NULL;
    spin_unlock(&g_driver_lock);
    irq_restore(flags);
    driver_drain();
    if (g_dhcp_active) {
        dhcp_stop(&g_netif);
        dhcp_cleanup(&g_netif);
        g_dhcp_active = false;
    }
    netif_set_down(&g_netif);
    netif_remove(&g_netif);
    g_local_device = NULL;
    spin_unlock(&g_lifecycle_lock);
}

bool net_driver_send(const uint8_t *data, uint16_t len) {
    if (!data || len < 14 || len > 1514) return false;
    const net_driver_ops_t *ops = driver_acquire(true, false);
    if (!ops) return false;
    bool sent = ops->send(data, len);
    driver_release();
    return sent;
}

bool net_driver_mac(uint8_t mac[6]) {
    if (!mac) return false;
    const net_driver_ops_t *ops = driver_acquire(false, false);
    if (!ops) return false;
    const uint8_t *address = ops->mac();
    bool valid = address != NULL;
    if (valid) memcpy(mac, address, 6);
    driver_release();
    return valid;
}

void net_receive(const uint8_t *frame, uint16_t len) {
    const net_driver_ops_t *ops = driver_acquire(true, true);
    if (!ops) return;
    kyronix_netif_input(&g_netif, frame, len);
    driver_release();
}

void net_receive_device(netdev_t *nd, const uint8_t *frame, uint16_t len) {
    if (!nd) return;
    const net_driver_ops_t *ops = driver_acquire(true, true);
    if (!ops) return;
    if (ops == &g_local_ops && nd == g_local_device)
        kyronix_netif_input(&g_netif, frame, len);
    driver_release();
}

void net_init(void) {
    netdev_t *nd = netdev_first();
    if (nd) register_driver(&g_local_ops, nd);
}

void net_poll(void) {
    net_schedule_poll();
}

void net_schedule_poll(void) {
    __atomic_store_n(&g_poll_pending, 1, __ATOMIC_RELEASE);
    proc_t *worker = __atomic_load_n(&g_net_worker, __ATOMIC_ACQUIRE);
    if (worker && __sync_bool_compare_and_swap(&worker->state, PROC_WAITING, PROC_READY))
        proc_set_ready(worker);
}
