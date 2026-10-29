#pragma once

#include <stdint.h>
#include <stdbool.h>

struct proc;

int64_t sys_getpid(void);
int64_t sys_getppid(void);
int64_t sys_getuid(void);
int64_t sys_getgid(void);
int64_t sys_geteuid(void);
int64_t sys_getegid(void);
int64_t sys_setfsuid(uint32_t uid);
int64_t sys_setfsgid(uint32_t gid);
int64_t sys_setuid(uint32_t uid);
int64_t sys_setgid(uint32_t gid);
int64_t sys_setreuid(uint32_t ruid, uint32_t euid);
int64_t sys_setregid(uint32_t rgid, uint32_t egid);
int64_t sys_setresuid(uint32_t ruid, uint32_t euid, uint32_t suid);
int64_t sys_setresgid(uint32_t rgid, uint32_t egid, uint32_t sgid);
int64_t sys_getresuid(uint32_t *ruid, uint32_t *euid, uint32_t *suid);
int64_t sys_getresgid(uint32_t *rgid, uint32_t *egid, uint32_t *sgid);
int64_t sys_getpgrp(void);
int64_t sys_getpgid(uint64_t pid);
int64_t sys_setsid(void);
int64_t sys_setpgid(uint64_t pid, uint64_t pgid);
int64_t sys_gettid(void);
int64_t sys_getgroups(int size, uint32_t *list);
int64_t sys_setgroups(int size, const uint32_t *list);
int64_t sys_capget(void *header, void *data);
int64_t sys_capset(void *header, const void *data);
bool cred_has_cap(struct proc *p, uint32_t cap);
void cred_drop_caps_for_uid(struct proc *p, uint32_t old_euid, uint32_t new_euid);
