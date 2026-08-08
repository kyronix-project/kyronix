#include "inet_socket.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../proc/proc.h"
#include "../proc/jail.h"
#include "security/phantom.h"
#include "../syscall/poll.h"
#include "../syscall/syscall.h"
#include "vfs.h"
#include "vfs_internal.h"

#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/raw.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"

#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define EINVAL 22
#define EMFILE 24
#define EMSGSIZE 90
#define ENOTSUP 95
#define ENOTCONN 107
#define EPERM 1
#define ECONNREFUSED 111
#define EINPROGRESS 115

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3

#define RX_BUF_SZ 16384u

typedef struct {
    uint8_t buf[RX_BUF_SZ];
    uint32_t head, tail;
} rx_ring_t;

static uint32_t ring_avail(const rx_ring_t *r) { return (r->head - r->tail) & (RX_BUF_SZ - 1); }

static void ring_push(rx_ring_t *r, const uint8_t *data, uint32_t len) {
    uint32_t free = (RX_BUF_SZ - 1u) - ring_avail(r);
    if (len > free) len = free;
    uint32_t first = RX_BUF_SZ - r->head;
    if (first > len) first = len;
    memcpy(r->buf + r->head, data, first);
    memcpy(r->buf, data + first, len - first);
    r->head = (r->head + len) & (RX_BUF_SZ - 1u);
}

static uint32_t ring_pop(rx_ring_t *r, uint8_t *out, uint32_t want) {
    uint32_t have = ring_avail(r);
    if (want > have) want = have;
    uint32_t first = RX_BUF_SZ - r->tail;
    if (first > want) first = want;
    memcpy(out, r->buf + r->tail, first);
    memcpy(out + first, r->buf, want - first);
    r->tail = (r->tail + want) & (RX_BUF_SZ - 1u);
    return want;
}

#define UDP_RX_SLOTS 8
#define UDP_DGRAM_SZ 2048
#define DGRAM_MAX_LEN 65535U
typedef struct {
    uint8_t data[UDP_DGRAM_SZ];
    uint16_t len;
    uint32_t src_ip;
    uint16_t src_port;
} udp_dgram_t;

struct net_conn {
    uint32_t refcount;
    int type;             /* SOCK_STREAM, SOCK_DGRAM, or SOCK_RAW */
    int proto;            /* IP protocol (SOCK_RAW only) */
    struct tcp_pcb *pcb;  /* tcp only */
    struct udp_pcb *upcb; /* udp only */
    struct raw_pcb *rpcb; /* raw only */
    bool is_server;
    bool phantom_fake;
    bool peer_closed;
    bool error;
    int err_code;

    rx_ring_t rx; /* tcp byte stream */

    udp_dgram_t udq[UDP_RX_SLOTS];
    int udq_head, udq_tail; /* head=write, tail=read */

    struct net_conn *accept_head;
    struct net_conn *accept_next;
    spinlock_t accept_lock;

    proc_t *rx_waiter;
    proc_t *connect_waiter;
    proc_t *accept_waiter;
};

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    net_conn_t *c = (net_conn_t *) arg;
    if (!c) return ERR_OK;

    if (!p || err != ERR_OK) {
        c->peer_closed = true;
        if (p) pbuf_free(p);
        proc_t *w = c->rx_waiter;
        if (w && __sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
            proc_set_ready(w);
        poll_notify_object(c);
        return ERR_OK;
    }

    uint32_t freelen = (RX_BUF_SZ - 1u) - ring_avail(&c->rx);
    if (p->tot_len > freelen) {
        proc_t *w = c->rx_waiter;
        if (w && __sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
            proc_set_ready(w);
        poll_notify_object(c);
        return ERR_MEM;
    }

    struct pbuf *q = p;
    while (q) {
        ring_push(&c->rx, (const uint8_t *) q->payload, q->len);
        q = q->next;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    proc_t *w = c->rx_waiter;
    if (w && __sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY)) proc_set_ready(w);
    poll_notify_object(c);
    return ERR_OK;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void) pcb;
    net_conn_t *c = (net_conn_t *) arg;
    if (!c) return ERR_OK;
    if (err != ERR_OK) {
        c->error = true;
        c->err_code = -(int) ECONNREFUSED;
        phantom_record(PHANTOM_EVENT_NETWORK, (uint32_t) err, 0, 0, "TCP connect failed");
    }
    proc_t *w = c->connect_waiter;
    if (w && __sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY)) proc_set_ready(w);
    poll_notify_object(c);
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void) err;
    net_conn_t *c = (net_conn_t *) arg;
    if (!c) return;
    c->error = true;
    c->err_code = -(int) ECONNREFUSED;
    c->pcb = NULL; /* already freed by lwIP */
    proc_t *w = c->connect_waiter;
    if (w && __sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY)) proc_set_ready(w);
    proc_t *r = c->rx_waiter;
    if (r && __sync_bool_compare_and_swap(&r->state, PROC_WAITING, PROC_READY)) proc_set_ready(r);
    poll_notify_object(c);
}

static err_t on_accept(void *arg, struct tcp_pcb *new_pcb, err_t err) {
    (void) err;
    net_conn_t *srv = (net_conn_t *) arg;
    if (!srv || !new_pcb) return ERR_VAL;

    net_conn_t *child = (net_conn_t *) kcalloc(1, sizeof(net_conn_t));
    if (!child) {
        tcp_abort(new_pcb);
        return ERR_MEM;
    }

    child->type = SOCK_STREAM;
    child->refcount = 1;
    child->pcb = new_pcb;
    tcp_arg(new_pcb, child);
    tcp_recv(new_pcb, on_recv);
    tcp_err(new_pcb, on_err);

    child->accept_next = NULL;
    spin_lock(&srv->accept_lock);
    if (!srv->accept_head) {
        srv->accept_head = child;
    } else {
        net_conn_t *tail = srv->accept_head;
        while (tail->accept_next) tail = tail->accept_next;
        tail->accept_next = child;
    }

    proc_t *w = srv->accept_waiter;
    if (w && __sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY)) proc_set_ready(w);
    spin_unlock(&srv->accept_lock);

    tcp_accepted(srv->pcb);
    poll_notify_object(srv);
    return ERR_OK;
}

static void on_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip4_addr_t *addr,
                        u16_t port) {
    (void) pcb;
    net_conn_t *c = (net_conn_t *) arg;
    if (!c || !p) {
        if (p) pbuf_free(p);
        return;
    }

    int next = (c->udq_head + 1) & (UDP_RX_SLOTS - 1);
    if (next == c->udq_tail) {
        pbuf_free(p);
        return;
    }

    udp_dgram_t *slot = &c->udq[c->udq_head];
    uint16_t copy = (p->tot_len < UDP_DGRAM_SZ) ? (uint16_t) p->tot_len : (uint16_t) (UDP_DGRAM_SZ);
    pbuf_copy_partial(p, slot->data, copy, 0);
    slot->len = copy;
    slot->src_ip = addr->addr;
    slot->src_port = port;
    pbuf_free(p);

    c->udq_head = next;
    proc_t *w = c->rx_waiter;
    if (w && __sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY)) proc_set_ready(w);
    poll_notify_object(c);
}

static uint8_t on_raw_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip4_addr_t *addr) {
    (void) pcb;
    net_conn_t *c = (net_conn_t *) arg;
    if (!c || !p) return 0;

    int next = (c->udq_head + 1) & (UDP_RX_SLOTS - 1);
    if (next == c->udq_tail) { return 0; } /* queue full, leave for others */

    udp_dgram_t *slot = &c->udq[c->udq_head];
    /* p->payload points at IP header for raw sockets */
    uint16_t copy = (p->tot_len < UDP_DGRAM_SZ) ? (uint16_t) p->tot_len : (uint16_t) UDP_DGRAM_SZ;
    pbuf_copy_partial(p, slot->data, copy, 0);
    slot->len = copy;
    slot->src_ip = addr->addr;
    slot->src_port = 0;

    c->udq_head = next;
    pbuf_free(p);
    proc_t *w = c->rx_waiter;
    if (w && __sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY)) proc_set_ready(w);
    poll_notify_object(c);
    return 1; /* consumed */
}

int fd_inet_socket(int type, int flags) {
    int sock_type = type & 0xF;
    if (sock_type != SOCK_STREAM && sock_type != SOCK_DGRAM && sock_type != SOCK_RAW)
        return -(int) ENOTSUP;
    if (sock_type == SOCK_RAW && !jail_host_priv(g_current_proc)) return -(int) EPERM;

    net_conn_t *c = (net_conn_t *) kcalloc(1, sizeof(net_conn_t));
    if (!c) return -(int) ENOMEM;
    c->type = sock_type;
    c->refcount = 1;
    c->proto = flags; /* proto arg passed as flags for SOCK_RAW */

    if (sock_type == SOCK_STREAM) {
        c->pcb = tcp_new();
        if (!c->pcb) {
            kfree(c);
            return -(int) ENOMEM;
        }
        tcp_arg(c->pcb, c);
    } else if (sock_type == SOCK_DGRAM) {
        c->upcb = udp_new();
        if (!c->upcb) {
            kfree(c);
            return -(int) ENOMEM;
        }
        udp_recv(c->upcb, on_udp_recv, c);
    } else {
        c->rpcb = raw_new((uint8_t) (flags & 0xFF));
        if (!c->rpcb) {
            kfree(c);
            return -(int) ENOMEM;
        }
        raw_recv(c->rpcb, on_raw_recv, c);
    }

    int fd = vfs_fd_alloc_from(0);
    if (fd < 0) {
        if (sock_type == SOCK_STREAM)
            tcp_abort(c->pcb);
        else if (sock_type == SOCK_DGRAM)
            udp_remove(c->upcb);
        else
            raw_remove(c->rpcb);
        kfree(c);
        return -(int) EMFILE;
    }

    vfs_file_t *f = vfs_file_alloc();
    if (!f) {
        vfs_fd_clear(fd);
        if (sock_type == SOCK_STREAM)
            tcp_abort(c->pcb);
        else if (sock_type == SOCK_DGRAM)
            udp_remove(c->upcb);
        else
            raw_remove(c->rpcb);
        kfree(c);
        return -(int) ENOMEM;
    }
    f->flags = O_RDWR | (type & O_NONBLOCK);
    f->cloexec = (type & O_CLOEXEC) ? 1 : 0;
    f->inet = c;
    vfs_fd_install(fd, f);
    return fd;
}

int64_t inet_fd_read(net_conn_t *c, void *buf, uint64_t len, int fd_flags) {
    if (!c) return -(int64_t) ENOTCONN;
    bool block = !(fd_flags & O_NONBLOCK);

    if (c->type == SOCK_DGRAM || c->type == SOCK_RAW) {
        while (c->udq_head == c->udq_tail) {
            if (!block) return -(int64_t) EAGAIN;
            c->rx_waiter = g_current_proc;
            sched_yield_blocking();
            c->rx_waiter = NULL;
        }
        udp_dgram_t *slot = &c->udq[c->udq_tail];
        uint64_t copy = (len < slot->len) ? len : (uint64_t) slot->len;
        memcpy(buf, slot->data, copy);
        c->udq_tail = (c->udq_tail + 1) & (UDP_RX_SLOTS - 1);
        return (int64_t) copy;
    }

    /* SOCK_STREAM */
    while (ring_avail(&c->rx) == 0) {
        if (c->peer_closed || c->error || !c->pcb) return 0;
        if (!block) return -(int64_t) EAGAIN;
        c->rx_waiter = g_current_proc;
        sched_yield_blocking();
        c->rx_waiter = NULL;
    }
    return (int64_t) ring_pop(&c->rx, (uint8_t *) buf, (uint32_t) len);
}

int64_t inet_fd_write(net_conn_t *c, const void *buf, uint64_t len) {
    if (!c) return -(int64_t) ENOTCONN;
    if (c->phantom_fake) {
        phantom_record(PHANTOM_EVENT_NETWORK, 0, 0, len, "spoofed network write accepted");
        return (int64_t) len;
    }
    if (c->type == SOCK_DGRAM || c->type == SOCK_RAW) {
        if (len > DGRAM_MAX_LEN) return -(int64_t) EMSGSIZE;
    } else if (len > DGRAM_MAX_LEN) {
        len = DGRAM_MAX_LEN;
    }
    if (c->type == SOCK_DGRAM) {
        if (!c->upcb) return -(int64_t) ENOTCONN;
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t) len, PBUF_RAM);
        if (!p) return -(int64_t) ENOMEM;
        memcpy(p->payload, buf, len);
        err_t e = udp_send(c->upcb, p);
        pbuf_free(p);
        return (e == ERR_OK) ? (int64_t) len : -(int64_t) EAGAIN;
    }
    if (c->type == SOCK_RAW) {
        /* buf contains raw payload after IP header (ICMP data etc) */
        if (!c->rpcb) return -(int64_t) ENOTCONN;
        struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t) len, PBUF_RAM);
        if (!p) return -(int64_t) ENOMEM;
        memcpy(p->payload, buf, len);
        err_t e = raw_send(c->rpcb, p);
        pbuf_free(p);
        return (e == ERR_OK) ? (int64_t) len : -(int64_t) EAGAIN;
    }
    if (!c->pcb) return -(int64_t) ENOTCONN;
    err_t e = tcp_write(c->pcb, buf, (u16_t) len, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) return -(int64_t) EAGAIN;
    tcp_output(c->pcb);
    return (int64_t) len;
}

void inet_conn_close(net_conn_t *c) {
    if (!c) return;
    if (__atomic_fetch_sub(&c->refcount, 1, __ATOMIC_ACQ_REL) > 1) return;
    if (c->type == SOCK_DGRAM) {
        if (c->upcb) {
            udp_remove(c->upcb);
            c->upcb = NULL;
        }
    } else if (c->type == SOCK_RAW) {
        if (c->rpcb) {
            raw_remove(c->rpcb);
            c->rpcb = NULL;
        }
    } else {
        if (c->pcb) {
            tcp_arg(c->pcb, NULL);
            tcp_recv(c->pcb, NULL);
            tcp_err(c->pcb, NULL);
            tcp_close(c->pcb);
            c->pcb = NULL;
        }
    }
    kfree(c);
}

void inet_conn_addref(net_conn_t *c) {
    if (c) __atomic_fetch_add(&c->refcount, 1, __ATOMIC_RELAXED);
}

net_conn_t *inet_phantom_clone(net_conn_t *source) {
    if (!source) return NULL;
    net_conn_t *fake = (net_conn_t *) kcalloc(1, sizeof(net_conn_t));
    if (!fake) return NULL;
    fake->refcount = 1;
    fake->type = source->type;
    fake->proto = source->proto;
    fake->phantom_fake = true;
    return fake;
}

bool inet_poll_in(net_conn_t *c) {
    if (!c) return false;
    if (c->type == SOCK_DGRAM || c->type == SOCK_RAW) return c->udq_head != c->udq_tail;
    return ring_avail(&c->rx) > 0 || c->peer_closed || c->error;
}

bool inet_poll_out(net_conn_t *c) {
    if (!c) return false;
    if (c->type == SOCK_DGRAM) return c->upcb != NULL;
    if (c->type == SOCK_RAW) return c->rpcb != NULL;
    return c->phantom_fake || c->error ||
           (c->pcb && c->pcb->state == ESTABLISHED);
}

int inet_get_type(net_conn_t *c) { return c ? c->type : 1; }

int inet_get_error(net_conn_t *c) {
    if (!c || !c->error) return 0;
    return c->err_code < 0 ? -c->err_code : c->err_code;
}

int64_t inet_connect(net_conn_t *c, const struct sockaddr_in *addr, int fd_flags) {
    if (!c) return -(int64_t) EBADF;

    if (g_current_proc && g_current_proc->phantom_sandbox) {
        c->phantom_fake = true;
        phantom_record(PHANTOM_EVENT_NETWORK, 0, addr->sin_addr, lwip_ntohs(addr->sin_port),
                       "spoofed network acknowledgment");
        return 0;
    }

    ip4_addr_t dst;
    dst.addr = addr->sin_addr;
    uint16_t port = lwip_ntohs(addr->sin_port);

    if (c->type == SOCK_DGRAM) {
        if (!c->upcb) return -(int64_t) EBADF;
        err_t e = udp_connect(c->upcb, &dst, port);
        return (e == ERR_OK) ? 0 : -(int64_t) EINVAL;
    }

    if (!c->pcb) return -(int64_t) EBADF;
    tcp_recv(c->pcb, on_recv);
    tcp_err(c->pcb, on_err);

    err_t e = tcp_connect(c->pcb, &dst, port, on_connected);
    if (e != ERR_OK) return -(int64_t) ECONNREFUSED;
    if (fd_flags & O_NONBLOCK) return -(int64_t) EINPROGRESS;

    while (!c->error && c->pcb && c->pcb->state != ESTABLISHED) {
        c->connect_waiter = g_current_proc;
        sched_yield_blocking();
        c->connect_waiter = NULL;
    }

    if (c->error) return (int64_t) c->err_code;
    phantom_record(PHANTOM_EVENT_NETWORK, 0, addr->sin_addr, port, "TCP connect acknowledged");
    return 0;
}

int64_t inet_bind(net_conn_t *c, const struct sockaddr_in *addr) {
    if (!c) return -(int64_t) EBADF;
    ip4_addr_t ip;
    ip.addr = addr->sin_addr;
    uint16_t port = lwip_ntohs(addr->sin_port);

    if (c->type == SOCK_DGRAM) {
        if (!c->upcb) return -(int64_t) EBADF;
        err_t e = udp_bind(c->upcb, &ip, port);
        return (e == ERR_OK) ? 0 : -(int64_t) EINVAL;
    }

    if (!c->pcb) return -(int64_t) EBADF;
    err_t e = tcp_bind(c->pcb, &ip, port);
    return (e == ERR_OK) ? 0 : -(int64_t) EINVAL;
}

int64_t inet_listen(net_conn_t *c, int backlog) {
    if (!c || !c->pcb || c->type != SOCK_STREAM) return -(int64_t) EBADF;
    (void) backlog;

    struct tcp_pcb *lpcb = tcp_listen(c->pcb);
    if (!lpcb) return -(int64_t) ENOMEM;
    c->pcb = lpcb;
    c->is_server = true;
    tcp_arg(c->pcb, c);
    tcp_accept(c->pcb, on_accept);
    return 0;
}

int64_t inet_accept(net_conn_t *c, struct sockaddr_in *addr_out, int flags) {
    if (!c || !c->is_server) return -(int64_t) EBADF;

    while (!c->accept_head) {
        if (flags & O_NONBLOCK) return -(int64_t) EAGAIN;
        c->accept_waiter = g_current_proc;
        sched_yield_blocking();
        c->accept_waiter = NULL;
    }

    spin_lock(&c->accept_lock);
    net_conn_t *child = c->accept_head;
    c->accept_head = child->accept_next;
    child->accept_next = NULL;
    spin_unlock(&c->accept_lock);

    int fd = vfs_fd_alloc_from(0);
    if (fd < 0) {
        inet_conn_close(child);
        return -(int64_t) EMFILE;
    }

    vfs_file_t *f = vfs_file_alloc();
    if (!f) {
        vfs_fd_clear(fd);
        inet_conn_close(child);
        return -(int64_t) ENOMEM;
    }
    f->flags = O_RDWR | (flags & O_NONBLOCK);
    f->cloexec = (flags & O_CLOEXEC) ? 1 : 0;
    f->inet = child;
    vfs_fd_install(fd, f);

    if (addr_out && child->pcb) {
        addr_out->sin_family = AF_INET;
        addr_out->sin_port = lwip_htons(child->pcb->remote_port);
        addr_out->sin_addr = child->pcb->remote_ip.addr;
        memset(addr_out->sin_zero, 0, sizeof(addr_out->sin_zero));
    }
    return (int64_t) fd;
}

int64_t inet_getsockname(net_conn_t *c, struct sockaddr_in *out) {
    if (!c || !c->pcb) return -(int64_t) EBADF;
    out->sin_family = AF_INET;
    out->sin_port = lwip_htons(c->pcb->local_port);
    out->sin_addr = c->pcb->local_ip.addr;
    memset(out->sin_zero, 0, sizeof(out->sin_zero));
    return 0;
}

int64_t inet_getpeername(net_conn_t *c, struct sockaddr_in *out) {
    if (!c || !c->pcb) return -(int64_t) ENOTCONN;
    out->sin_family = AF_INET;
    out->sin_port = lwip_htons(c->pcb->remote_port);
    out->sin_addr = c->pcb->remote_ip.addr;
    memset(out->sin_zero, 0, sizeof(out->sin_zero));
    return 0;
}

int64_t inet_sendto(net_conn_t *c, const void *buf, uint64_t len, const struct sockaddr_in *addr) {
    if (!c) return -(int64_t) ENOTCONN;
    if ((c->type == SOCK_DGRAM || c->type == SOCK_RAW) && len > DGRAM_MAX_LEN)
        return -(int64_t) EMSGSIZE;

    if (c->type == SOCK_DGRAM) {
        if (!c->upcb) return -(int64_t) ENOTCONN;
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t) len, PBUF_RAM);
        if (!p) return -(int64_t) ENOMEM;
        memcpy(p->payload, buf, len);
        err_t e;
        if (addr) {
            ip4_addr_t dst;
            dst.addr = addr->sin_addr;
            uint16_t port = lwip_ntohs(addr->sin_port);
            e = udp_sendto(c->upcb, p, &dst, port);
        } else {
            e = udp_send(c->upcb, p);
        }
        pbuf_free(p);
        return (e == ERR_OK) ? (int64_t) len : -(int64_t) EAGAIN;
    }

    if (c->type == SOCK_RAW) {
        if (!c->rpcb) return -(int64_t) ENOTCONN;
        struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t) len, PBUF_RAM);
        if (!p) return -(int64_t) ENOMEM;
        memcpy(p->payload, buf, len);
        err_t e;
        if (addr) {
            ip4_addr_t dst;
            dst.addr = addr->sin_addr;
            ip_addr_t dstip;
            ip_addr_copy_from_ip4(dstip, dst);
            e = raw_sendto(c->rpcb, p, &dstip);
        } else {
            e = raw_send(c->rpcb, p);
        }
        pbuf_free(p);
        return (e == ERR_OK) ? (int64_t) len : -(int64_t) EAGAIN;
    }

    return inet_fd_write(c, buf, len);
}

int64_t inet_recvfrom(net_conn_t *c, void *buf, uint64_t len, struct sockaddr_in *addr_out,
                      int fd_flags) {
    if (!c) return -(int64_t) ENOTCONN;

    if (c->type == SOCK_DGRAM || c->type == SOCK_RAW) {
        bool block = !(fd_flags & O_NONBLOCK);
        while (c->udq_head == c->udq_tail) {
            if (!block) return -(int64_t) EAGAIN;
            c->rx_waiter = g_current_proc;
            sched_yield_blocking();
            c->rx_waiter = NULL;
        }
        udp_dgram_t *slot = &c->udq[c->udq_tail];
        uint64_t copy = (len < slot->len) ? len : (uint64_t) slot->len;
        memcpy(buf, slot->data, copy);
        if (addr_out) {
            addr_out->sin_family = AF_INET;
            addr_out->sin_addr = slot->src_ip;
            addr_out->sin_port = lwip_htons(slot->src_port);
            memset(addr_out->sin_zero, 0, sizeof(addr_out->sin_zero));
        }
        c->udq_tail = (c->udq_tail + 1) & (UDP_RX_SLOTS - 1);
        return (int64_t) copy;
    }

    int64_t r = inet_fd_read(c, buf, len, fd_flags);
    if (addr_out && r > 0 && c->pcb) {
        addr_out->sin_family = AF_INET;
        addr_out->sin_port = lwip_htons(c->pcb->remote_port);
        addr_out->sin_addr = c->pcb->remote_ip.addr;
        memset(addr_out->sin_zero, 0, sizeof(addr_out->sin_zero));
    }
    return r;
}
