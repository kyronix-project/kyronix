#include "kallsyms.h"
#include "lib/printf.h"
#include "lib/string.h"

static char g_fallback[32];

const char *kallsyms_lookup(uint64_t addr) {
    if (kallsyms_num <= 0) {
        snprintf(g_fallback, sizeof(g_fallback), "0x%016lx", addr);
        return g_fallback;
    }
    int lo = 0, hi = kallsyms_num - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (kallsyms_table[mid].addr <= addr) {
            if (mid == kallsyms_num - 1 || kallsyms_table[mid + 1].addr > addr)
                return kallsyms_table[mid].name;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    snprintf(g_fallback, sizeof(g_fallback), "0x%016lx", addr);
    return g_fallback;
}

uint64_t kallsyms_lookup_name(const char *name) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < kallsyms_num; i++)
        if (strcmp(kallsyms_table[i].name, name) == 0) return kallsyms_table[i].addr;
    return 0;
}
