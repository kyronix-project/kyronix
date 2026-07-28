#include "net.h"
#include "../arch/x86_64/pit.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../mm/heap.h"

#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/opt.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

err_t kyronix_netif_init(struct netif *nif);
void kyronix_netif_input(struct netif *nif, const uint8_t *data, uint16_t len);

static struct netif g_netif;
static spinlock_irqsave_t g_driver_lock;
static const net_driver_ops_t *g_driver;
static uint32_t g_driver_calls;
static bool g_lwip_initialized;
static bool g_netif_active;

static const net_driver_ops_t *driver_acquire(bool require_netif) {
    const net_driver_ops_t *ops = NULL;
    spin_lock_irqsave(&g_driver_lock);
    if (g_driver && (!require_netif || g_netif_active)) {
        ops = g_driver;
        g_driver_calls++;
    }
    spin_unlock_irqrestore(&g_driver_lock);
    return ops;
}

static void driver_release(void) {
    spin_lock_irqsave(&g_driver_lock);
    if (g_driver_calls) g_driver_calls--;
    spin_unlock_irqrestore(&g_driver_lock);
}

bool net_driver_register(const net_driver_ops_t *ops) {
    if (!ops || !ops->send || !ops->poll || !ops->mac) return false;
    spin_lock_irqsave(&g_driver_lock);
    if (g_driver) {
        spin_unlock_irqrestore(&g_driver_lock);
        return false;
    }
    g_driver = ops;
    spin_unlock_irqrestore(&g_driver_lock);

    if (!g_lwip_initialized) {
        lwip_init();
        dns_init();
        g_lwip_initialized = true;
    }
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);

    if (!netif_add(&g_netif, &ip, &mask, &gw, NULL, kyronix_netif_init, ethernet_input)) {
        spin_lock_irqsave(&g_driver_lock);
        g_driver = NULL;
        spin_unlock_irqrestore(&g_driver_lock);
        return false;
    }
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

    ip4_addr_t dns1;
    IP4_ADDR(&dns1, 10, 0, 2, 3);
    dns_setserver(0, &dns1);

    spin_lock_irqsave(&g_driver_lock);
    g_netif_active = true;
    spin_unlock_irqrestore(&g_driver_lock);
    log_info("net: lwIP initialized, IP 10.0.2.15/24 gw 10.0.2.2 dns 10.0.2.3");
    return true;
}

void net_driver_unregister(const net_driver_ops_t *ops) {
    spin_lock_irqsave(&g_driver_lock);
    if (g_driver != ops) {
        spin_unlock_irqrestore(&g_driver_lock);
        return;
    }
    g_netif_active = false;
    g_driver = NULL;
    spin_unlock_irqrestore(&g_driver_lock);

    while (__atomic_load_n(&g_driver_calls, __ATOMIC_ACQUIRE)) cpu_relax();
    netif_set_down(&g_netif);
    netif_remove(&g_netif);
}

bool net_driver_send(const uint8_t *data, uint16_t len) {
    const net_driver_ops_t *ops = driver_acquire(true);
    if (!ops) return false;
    bool sent = ops->send(data, len);
    driver_release();
    return sent;
}

bool net_driver_mac(uint8_t mac[6]) {
    const net_driver_ops_t *ops = driver_acquire(false);
    if (!ops) return false;
    const uint8_t *driver_mac = ops->mac();
    if (driver_mac) memcpy(mac, driver_mac, 6);
    driver_release();
    return driver_mac != NULL;
}

void net_receive(const uint8_t *eth_frame, uint16_t len) {
    if (__atomic_load_n(&g_netif_active, __ATOMIC_ACQUIRE))
        kyronix_netif_input(&g_netif, eth_frame, len);
}

void net_poll(void) {
    const net_driver_ops_t *ops = driver_acquire(true);
    if (ops) {
        ops->poll(); /* drain rx -> works even if irq does not fire on q35 */
        driver_release();
    }
    static uint8_t s_ctr;
    if (g_lwip_initialized && ++s_ctr == 0) sys_check_timeouts();
}
