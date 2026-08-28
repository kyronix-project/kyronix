#pragma once
#include <stdbool.h>
#include <stdint.h>

bool ahci_init(void);
bool ahci_ready(void);
int ahci_first_disk(void);
int ahci_port_count(void);
uint64_t ahci_disk_sectors(int port);
void ahci_disk_model(int port, char *buf, int len);
int ahci_read(int port, uint64_t lba, uint32_t count, void *buf);
int ahci_write(int port, uint64_t lba, uint32_t count, const void *buf);
int ahci_flush(int port);
