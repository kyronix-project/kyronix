#pragma once
#include <stdint.h>

struct sysinfo_s {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram;
    uint64_t totalswap, freeswap;
    uint16_t procs;
    uint8_t _pad[6];
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
};

int64_t sys_getrlimit(uint64_t r, void* rl);
int64_t sys_prlimit64(uint64_t p, uint64_t r, void* nl, void* ol);
int64_t sys_nanosleep(void* r, void* m);
int64_t sys_getitimer(int w, void* v);
int64_t sys_setitimer(int w, const void* n, void* o);
int64_t sys_clock_gettime(uint64_t c, void* t);
int64_t sys_gettimeofday(void* tv, void* tz);
int64_t sys_times(void* b);
int64_t sys_alarm(uint32_t seconds);
int64_t sys_clock_getres(uint64_t clk, void* res);
int64_t sys_sysinfo(struct sysinfo_s* si);
