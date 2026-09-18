#include "block.h"
#include "../lib/string.h"

#define BLOCK_MAX_DEVICES 32

static struct block_device *g_devices[BLOCK_MAX_DEVICES];
static int g_count = 0;

void block_init(void) {
    g_count = 0;
    for (int i = 0; i < BLOCK_MAX_DEVICES; i++) g_devices[i] = NULL;
}

void block_register(struct block_device *dev) {
    if (g_count >= BLOCK_MAX_DEVICES) return;
    g_devices[g_count++] = dev;
}

int block_count(void) { return g_count; }

struct block_device *block_get(int index) {
    if (index < 0 || index >= g_count) return NULL;
    return g_devices[index];
}

struct block_device *block_by_name(const char *name) {
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_devices[i]->name, name) == 0) return g_devices[i];
    }
    return NULL;
}

struct block_device *block_first(void) {
    if (g_count == 0) return NULL;
    return g_devices[0];
}
