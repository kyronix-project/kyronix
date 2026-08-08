#pragma once
#include <stdbool.h>
#include <stdint.h>

// static net cfg for now
#define NET_MY_IP 0x0A00020Fu /* 10.0.2.15  */
#define NET_MASK 0xFFFFFF00u  /* /24         */
#define NET_GW 0x0A000202u    /* 10.0.2.2   */

typedef struct {
    bool (*send)(const uint8_t *data, uint16_t len);
    void (*poll)(void);
    const uint8_t *(*mac)(void);
} net_driver_ops_t;

bool net_driver_register(const net_driver_ops_t *ops);
void net_driver_unregister(const net_driver_ops_t *ops);
bool net_driver_send(const uint8_t *data, uint16_t len);
bool net_driver_mac(uint8_t mac[6]);
void net_poll(void);
void net_schedule_poll(void);
void net_receive(const uint8_t *eth_frame, uint16_t len);
