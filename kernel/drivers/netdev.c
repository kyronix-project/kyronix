#include "netdev.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../net/net.h"

static netdev_t *g_netdevs;
static int g_netdev_count;
static spinlock_t g_netdev_lock;

void netdev_init(void) {
    uint64_t flags = irq_save();
    spin_lock(&g_netdev_lock);
    if (!g_netdev_count) g_netdevs = NULL;
    spin_unlock(&g_netdev_lock);
    irq_restore(flags);
}

int netdev_register(netdev_t *nd) {
    if (!nd || !nd->send || !nd->poll || !nd->name[0]) return -1;
    unsigned int name_len = 0;
    while (name_len < NETDEV_NAME_MAX && nd->name[name_len]) name_len++;
    if (name_len == NETDEV_NAME_MAX) return -1;
    uint8_t mac_bits = 0;
    for (unsigned int i = 0; i < sizeof(nd->mac); i++) mac_bits |= nd->mac[i];
    if (!mac_bits || (nd->mac[0] & 1u)) return -1;

    uint64_t flags = irq_save();
    spin_lock(&g_netdev_lock);
    if (g_netdev_count >= NETDEV_MAX) {
        spin_unlock(&g_netdev_lock);
        irq_restore(flags);
        return -1;
    }
    for (netdev_t *entry = g_netdevs; entry; entry = entry->next) {
        if (entry == nd || strcmp(entry->name, nd->name) == 0) {
            spin_unlock(&g_netdev_lock);
            irq_restore(flags);
            return -1;
        }
    }
    nd->next = g_netdevs;
    g_netdevs = nd;
    g_netdev_count++;
    spin_unlock(&g_netdev_lock);
    irq_restore(flags);
    log_info("netdev: registered %s MAC %02x:%02x:%02x:%02x:%02x:%02x", nd->name, nd->mac[0],
             nd->mac[1], nd->mac[2], nd->mac[3], nd->mac[4], nd->mac[5]);
    return 0;
}

int netdev_count(void) {
    uint64_t flags = irq_save();
    spin_lock(&g_netdev_lock);
    int count = g_netdev_count;
    spin_unlock(&g_netdev_lock);
    irq_restore(flags);
    return count;
}

netdev_t *netdev_get(int idx) {
    if (idx < 0) return NULL;
    uint64_t flags = irq_save();
    spin_lock(&g_netdev_lock);
    netdev_t *nd = idx < g_netdev_count ? g_netdevs : NULL;
    for (int i = 0; i < idx && nd; i++) nd = nd->next;
    spin_unlock(&g_netdev_lock);
    irq_restore(flags);
    return nd;
}

netdev_t *netdev_first(void) { return netdev_get(0); }

netdev_t *netdev_by_name(const char *name) {
    if (!name) return NULL;
    uint64_t flags = irq_save();
    spin_lock(&g_netdev_lock);
    netdev_t *nd = g_netdevs;
    while (nd && strcmp(nd->name, name) != 0) nd = nd->next;
    spin_unlock(&g_netdev_lock);
    irq_restore(flags);
    return nd;
}

void netdev_receive(netdev_t *nd, const uint8_t *frame, uint16_t len) {
    net_receive_device(nd, frame, len);
}

void netdev_poll_all(void) {
    netdev_t *devices[NETDEV_MAX];
    unsigned int count = 0;
    uint64_t flags = irq_save();
    spin_lock(&g_netdev_lock);
    for (netdev_t *nd = g_netdevs; nd && count < NETDEV_MAX; nd = nd->next)
        devices[count++] = nd;
    spin_unlock(&g_netdev_lock);
    irq_restore(flags);
    for (unsigned int i = 0; i < count; i++) devices[i]->poll(devices[i]);
}
