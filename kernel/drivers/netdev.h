#pragma once

#include <stdbool.h>
#include <stdint.h>

#define NETDEV_NAME_MAX 8
#define NETDEV_MAX 8

typedef struct netdev {
    char name[NETDEV_NAME_MAX];
    uint8_t mac[6];
    int (*send)(struct netdev *nd, const uint8_t *frame, uint16_t len);
    void (*poll)(struct netdev *nd);
    void *priv;
    struct netdev *next;
} netdev_t;

void netdev_init(void);
int netdev_register(netdev_t *nd);
int netdev_count(void);
netdev_t *netdev_get(int idx);
netdev_t *netdev_first(void);
netdev_t *netdev_by_name(const char *name);
void netdev_receive(netdev_t *nd, const uint8_t *frame, uint16_t len);
void netdev_poll_all(void);

void e1000_init(void);
void rtl8139_init(void);
void rtl8169_init(void);
void ath5k_init(void);
