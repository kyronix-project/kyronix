#pragma once
#include <stdint.h>

typedef struct {
    uint64_t addr;
    const char *name;
} sym_entry_t;

extern const sym_entry_t kallsyms_table[];
extern const int kallsyms_num;

const char *kallsyms_lookup(uint64_t addr);
uint64_t kallsyms_lookup_name(const char *name);
