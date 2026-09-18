#pragma once
#include <stdbool.h>
#include <stdint.h>

void virtnet_init(void);
void virtnet_poll(void);
bool virtnet_send(const uint8_t *data, uint16_t len);
const uint8_t *virtnet_mac(void);
bool virtnet_ready(void);
