#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct net_conn net_conn_t;

int fd_inet_socket(int type, int flags);
int inet_get_type(net_conn_t *c);

int64_t inet_fd_read(net_conn_t *c, void *buf, uint64_t len, int fd_flags);
int64_t inet_fd_write(net_conn_t *c, const void *buf, uint64_t len);
void inet_conn_close(net_conn_t *c);
void inet_conn_addref(net_conn_t *c);
net_conn_t *inet_phantom_clone(net_conn_t *source);
bool inet_poll_in(net_conn_t *c);
bool inet_poll_out(net_conn_t *c);

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port; /* big-endian */
    uint32_t sin_addr; /* big-endian */
    uint8_t sin_zero[8];
};

int64_t inet_connect(net_conn_t *c, const struct sockaddr_in *addr, int fd_flags);
int inet_get_error(net_conn_t *c);
int64_t inet_bind(net_conn_t *c, const struct sockaddr_in *addr);
int64_t inet_listen(net_conn_t *c, int backlog);
int64_t inet_accept(net_conn_t *c, struct sockaddr_in *addr_out, int flags);
int64_t inet_getsockname(net_conn_t *c, struct sockaddr_in *addr_out);
int64_t inet_getpeername(net_conn_t *c, struct sockaddr_in *addr_out);
int64_t inet_sendto(net_conn_t *c, const void *buf, uint64_t len, const struct sockaddr_in *addr);
int64_t inet_recvfrom(net_conn_t *c, void *buf, uint64_t len, struct sockaddr_in *addr_out,
                      int fd_flags);
