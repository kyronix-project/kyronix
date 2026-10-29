#pragma once
#include <stdbool.h>
#include <stdint.h>

struct proc;
struct seccomp_filter;

#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT 1
#define SECCOMP_MODE_FILTER 2

#define SECCOMP_SET_MODE_STRICT 0
#define SECCOMP_SET_MODE_FILTER 1
#define SECCOMP_GET_ACTION_AVAIL 2
#define SECCOMP_GET_NOTIF_SIZES 3
#define SECCOMP_FILTER_FLAG_TSYNC 1
#define SECCOMP_FILTER_FLAG_LOG 2
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW 4
#define SECCOMP_FILTER_FLAG_NEW_LISTENER 8

#define SECCOMP_RET_KILL_THREAD 0x00000000U
#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_TRAP 0x00030000U
#define SECCOMP_RET_ERRNO 0x00050000U
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#define SECCOMP_RET_TRACE 0x7ff00000U
#define SECCOMP_RET_LOG 0x7ffc0000U
#define SECCOMP_RET_ALLOW 0x7fff0000U

typedef struct {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
} seccomp_insn_t;

typedef struct {
    uint16_t len;
    uint64_t filter;
} seccomp_fprog_t;

struct seccomp_filter *seccomp_build(const seccomp_insn_t *insns, uint16_t len);
struct seccomp_filter *seccomp_chain_clone(const struct seccomp_filter *head);
void seccomp_chain_free(struct seccomp_filter *head);
void seccomp_install(struct proc *p, struct seccomp_filter *filter);
bool seccomp_check(struct proc *p, uint64_t nr, const uint64_t args[6], int64_t *ret);
