#pragma once

#include <stdbool.h>
#include <stdint.h>

void acpi_init(uint64_t rsdp_phys);
bool acpi_available(void);
uint64_t acpi_rsdp_phys(void);
__attribute__((noreturn)) void acpi_poweroff(void);
__attribute__((noreturn)) void acpi_reboot(void);
