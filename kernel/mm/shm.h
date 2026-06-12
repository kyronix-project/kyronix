#pragma once
#include <stdint.h>

int      sys_shmget(int key, uint64_t size, int flags);
uint64_t sys_shmat(int shmid, uint64_t addr, int shmflg);
int      sys_shmdt(uint64_t addr);
int      sys_shmctl(int shmid, int cmd, void* buf);
void     shm_proc_exit(uint32_t pid);
