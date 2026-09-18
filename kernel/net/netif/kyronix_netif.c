#include <stdbool.h>
#include <stdint.h>

#include "lwip/def.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/opt.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#include "../../lib/log.h"
#include "../../lib/string.h"
#include "../net.h"

void kyronix_netif_input(struct netif *nif, const uint8_t *data, uint16_t len) {
    if (!nif || !nif->input || !data || len < 14 || len > 1514) return;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
    if (!p) return;
    if (pbuf_take(p, data, len) != ERR_OK) {
        pbuf_free(p);
        return;
    }
    if (nif->input(p, nif) != ERR_OK) pbuf_free(p);
}

static err_t kyronix_netif_output(struct netif *nif, struct pbuf *p) {
    (void) nif;
    if (!p || p->tot_len < 14) return ERR_ARG;
    uint8_t buf[1514];
    if (p->tot_len > sizeof(buf)) return ERR_BUF;
    if (pbuf_copy_partial(p, buf, p->tot_len, 0) != p->tot_len) return ERR_BUF;
    return net_driver_send(buf, p->tot_len) ? ERR_OK : ERR_IF;
}

err_t kyronix_netif_init(struct netif *nif) {
    if (!nif) return ERR_ARG;
    uint8_t mac[6];
    if (!net_driver_mac(mac)) return ERR_IF;

    nif->name[0] = 'e';
    nif->name[1] = '0';
    nif->output = etharp_output;
    nif->linkoutput = kyronix_netif_output;
    nif->mtu = 1500;
    nif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    memcpy(nif->hwaddr, mac, sizeof(mac));
    nif->hwaddr_len = sizeof(mac);

    log_info("net: kyronix netif init, MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
             mac[3], mac[4], mac[5]);
    return ERR_OK;
}
