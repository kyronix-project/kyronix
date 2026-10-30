// SPDX-License-Identifier: GPL-2.0
#include "seccomp.h"

#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include "proc/signal.h"

#define BPF_LD 0x00
#define BPF_LDX 0x01
#define BPF_ST 0x02
#define BPF_STX 0x03
#define BPF_ALU 0x04
#define BPF_JMP 0x05
#define BPF_RET 0x06
#define BPF_MISC 0x07

#define BPF_W 0x00
#define BPF_H 0x08
#define BPF_B 0x10

#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_IND 0x40
#define BPF_MEM 0x60
#define BPF_LEN 0x80
#define BPF_MSH 0xa0

#define BPF_ADD 0x00
#define BPF_SUB 0x10
#define BPF_MUL 0x20
#define BPF_DIV 0x30
#define BPF_OR 0x40
#define BPF_AND 0x50
#define BPF_LSH 0x60
#define BPF_RSH 0x70
#define BPF_NEG 0x80
#define BPF_MOD 0x90
#define BPF_XOR 0xa0

#define BPF_JA 0x00
#define BPF_JEQ 0x10
#define BPF_JGT 0x20
#define BPF_JGE 0x30
#define BPF_JSET 0x40

#define BPF_K 0x00
#define BPF_X 0x08

#define BPF_TAX 0x00
#define BPF_TXA 0x80

#define SECCOMP_MAX_INSNS 4096
#define SECCOMP_DATA_SIZE 64
#define ENOSYS 38

struct seccomp_filter {
    uint16_t len;
    struct seccomp_filter *next;
    seccomp_insn_t insns[];
};

static uint32_t bpf_load(const uint8_t *data, uint32_t off, uint32_t size) {
    uint32_t value = 0;

    if (off > SECCOMP_DATA_SIZE || size > SECCOMP_DATA_SIZE - off) {
        return 0;
    }

    for (uint32_t i = 0; i < size; i++) {
        value |= (uint32_t) data[off + i] << (i * 8);
    }

    return value;
}

static void bpf_store(uint8_t *data, uint32_t off, uint32_t size, uint64_t value) {
    if (off > SECCOMP_DATA_SIZE || size > SECCOMP_DATA_SIZE - off) {
        return;
    }

    for (uint32_t i = 0; i < size; i++) {
        data[off + i] = (uint8_t) (value >> (i * 8));
    }
}

static uint32_t bpf_run(const struct seccomp_filter *filter, const uint8_t *data) {
    uint32_t a = 0;
    uint32_t x = 0;
    uint32_t mem[16];
    uint32_t pc = 0;

    memset(mem, 0, sizeof(mem));

    while (pc < filter->len) {
        const seccomp_insn_t *insn = &filter->insns[pc];
        uint32_t code = insn->code;
        uint32_t cls = code & 0x07;
        uint32_t size = code & 0x18;
        uint32_t mode = code & 0xe0;
        uint32_t op = code & 0xf0;
        uint32_t src = code & 0x08;
        uint32_t val = 0;

        switch (cls) {
        case BPF_LD:
            if (mode == BPF_IMM) {
                val = insn->k;
            } else if (mode == BPF_ABS) {
                val = bpf_load(data, insn->k, size == BPF_W ? 4 : size == BPF_H ? 2 : 1);
            } else if (mode == BPF_IND) {
                val = bpf_load(data, x + insn->k, size == BPF_W ? 4 : size == BPF_H ? 2 : 1);
            } else if (mode == BPF_MEM) {
                if (insn->k >= 16) return SECCOMP_RET_KILL_PROCESS;
                val = mem[insn->k];
            } else if (mode == BPF_LEN) {
                val = SECCOMP_DATA_SIZE;
            } else {
                return SECCOMP_RET_KILL_PROCESS;
            }
            a = val;
            pc++;
            break;
        case BPF_LDX:
            if (mode == BPF_IMM) {
                x = insn->k;
            } else if (mode == BPF_MEM) {
                if (insn->k >= 16) return SECCOMP_RET_KILL_PROCESS;
                x = mem[insn->k];
            } else if (mode == BPF_LEN) {
                x = SECCOMP_DATA_SIZE;
            } else {
                return SECCOMP_RET_KILL_PROCESS;
            }
            pc++;
            break;
        case BPF_ST:
            if (insn->k >= 16) return SECCOMP_RET_KILL_PROCESS;
            mem[insn->k] = a;
            pc++;
            break;
        case BPF_STX:
            if (insn->k >= 16) return SECCOMP_RET_KILL_PROCESS;
            mem[insn->k] = x;
            pc++;
            break;
        case BPF_ALU: {
            uint32_t rhs = src == BPF_X ? x : insn->k;

            switch (op) {
            case BPF_ADD: a += rhs; break;
            case BPF_SUB: a -= rhs; break;
            case BPF_MUL: a *= rhs; break;
            case BPF_DIV:
                if (rhs == 0) return SECCOMP_RET_KILL_PROCESS;
                a /= rhs;
                break;
            case BPF_OR: a |= rhs; break;
            case BPF_AND: a &= rhs; break;
            case BPF_LSH: a <<= rhs; break;
            case BPF_RSH: a >>= rhs; break;
            case BPF_NEG: a = (uint32_t) -a; break;
            case BPF_MOD:
                if (rhs == 0) return SECCOMP_RET_KILL_PROCESS;
                a %= rhs;
                break;
            case BPF_XOR: a ^= rhs; break;
            default: return SECCOMP_RET_KILL_PROCESS;
            }
            pc++;
            break;
        }
        case BPF_JMP:
            if (op == BPF_JA) {
                pc += 1 + insn->k;
                break;
            }

            {
                uint32_t rhs = src == BPF_X ? x : insn->k;
                bool take;

                switch (op) {
                case BPF_JEQ: take = a == rhs; break;
                case BPF_JGT: take = a > rhs; break;
                case BPF_JGE: take = a >= rhs; break;
                case BPF_JSET: take = (a & rhs) != 0; break;
                default: return SECCOMP_RET_KILL_PROCESS;
                }
                pc += 1 + (take ? insn->jt : insn->jf);
            }
            break;
        case BPF_RET:
            if (op == BPF_K) return insn->k;
            if (op == 0x10) return a;
            return SECCOMP_RET_KILL_PROCESS;
        case BPF_MISC:
            if (op == BPF_TAX) {
                x = a;
            } else if (op == BPF_TXA) {
                a = x;
            } else {
                return SECCOMP_RET_KILL_PROCESS;
            }
            pc++;
            break;
        default:
            return SECCOMP_RET_KILL_PROCESS;
        }
    }

    return SECCOMP_RET_KILL_PROCESS;
}

static bool bpf_valid(const seccomp_insn_t *insns, uint16_t len) {
    if (!insns || len == 0 || len > SECCOMP_MAX_INSNS) {
        return false;
    }

    for (uint32_t pc = 0; pc < len; pc++) {
        const seccomp_insn_t *insn = &insns[pc];
        uint32_t cls = insn->code & 0x07;

        if (cls == BPF_JMP) {
            uint32_t op = insn->code & 0xf0;
            if (op == BPF_JA) {
                if (pc + 1 + insn->k >= len) return false;
            } else if (op != BPF_JEQ && op != BPF_JGT && op != BPF_JGE && op != BPF_JSET) {
                return false;
            } else {
                if (pc + 1 + insn->jt >= len) return false;
                if (pc + 1 + insn->jf >= len) return false;
            }
        } else if (cls != BPF_LD && cls != BPF_LDX && cls != BPF_ST && cls != BPF_STX &&
                   cls != BPF_ALU && cls != BPF_RET && cls != BPF_MISC) {
            return false;
        }
    }

    return true;
}

struct seccomp_filter *seccomp_build(const seccomp_insn_t *insns, uint16_t len) {
    if (!bpf_valid(insns, len)) {
        return NULL;
    }

    uint64_t bytes = sizeof(struct seccomp_filter) + (uint64_t) len * sizeof(seccomp_insn_t);
    struct seccomp_filter *filter = (struct seccomp_filter *) kmalloc(bytes);
    if (!filter) {
        return NULL;
    }

    filter->len = len;
    filter->next = NULL;
    memcpy(filter->insns, insns, (uint64_t) len * sizeof(seccomp_insn_t));
    return filter;
}

struct seccomp_filter *seccomp_chain_clone(const struct seccomp_filter *head) {
    struct seccomp_filter *copy = NULL;
    struct seccomp_filter **tail = &copy;

    for (const struct seccomp_filter *f = head; f; f = f->next) {
        struct seccomp_filter *node = seccomp_build(f->insns, f->len);
        if (!node) {
            seccomp_chain_free(copy);
            return NULL;
        }
        *tail = node;
        tail = &node->next;
    }

    return copy;
}

void seccomp_chain_free(struct seccomp_filter *head) {
    while (head) {
        struct seccomp_filter *next = head->next;
        kfree(head);
        head = next;
    }
}

void seccomp_install(struct proc *p, struct seccomp_filter *filter) {
    if (!p || !filter) {
        return;
    }

    filter->next = p->seccomp_filter;
    p->seccomp_filter = filter;
    p->seccomp_mode = SECCOMP_MODE_FILTER;
}

static int action_rank(uint32_t action) {
    switch (action) {
    case SECCOMP_RET_KILL_PROCESS:
    case SECCOMP_RET_KILL_THREAD:
        return 0;
    case SECCOMP_RET_TRAP:
        return 1;
    case SECCOMP_RET_ERRNO:
        return 2;
    case SECCOMP_RET_USER_NOTIF:
        return 3;
    case SECCOMP_RET_TRACE:
        return 4;
    case SECCOMP_RET_LOG:
        return 5;
    case SECCOMP_RET_ALLOW:
        return 6;
    default:
        return 0;
    }
}

static uint32_t chain_result(const struct seccomp_filter *head, const uint8_t *data) {
    uint32_t best = SECCOMP_RET_ALLOW;

    for (const struct seccomp_filter *f = head; f; f = f->next) {
        uint32_t result = bpf_run(f, data);
        if (action_rank(result & 0xffff0000U) < action_rank(best & 0xffff0000U)) {
            best = result;
        }
    }

    return best;
}

bool seccomp_check(struct proc *p, uint64_t nr, const uint64_t args[6], int64_t *ret) {
    if (!p || p->seccomp_mode == SECCOMP_MODE_DISABLED) {
        return false;
    }

    if (p->seccomp_mode == SECCOMP_MODE_STRICT) {
        if (nr == 0 || nr == 1 || nr == 15 || nr == 60 || nr == 231) {
            return false;
        }
        proc_do_exit(-SIGSYS);
    }

    uint8_t data[SECCOMP_DATA_SIZE];
    memset(data, 0, sizeof(data));
    bpf_store(data, 0, 4, (uint32_t) nr);
    bpf_store(data, 4, 4, 0xc000003eU);
    for (int i = 0; i < 6; i++) {
        bpf_store(data, 16 + (uint32_t) i * 8, 8, args[i]);
    }

    uint32_t result = chain_result(p->seccomp_filter, data);
    uint32_t action = result & 0xffff0000U;
    uint32_t value = result & 0x0000ffffU;

    switch (action) {
    case SECCOMP_RET_ALLOW:
    case SECCOMP_RET_LOG:
        return false;
    case SECCOMP_RET_ERRNO:
        if (ret) *ret = -(int64_t) value;
        return true;
    case SECCOMP_RET_TRAP:
        proc_send_signal(p, SIGSYS);
        if (ret) *ret = -(int64_t) ENOSYS;
        return true;
    case SECCOMP_RET_TRACE:
        if (ret) *ret = -(int64_t) ENOSYS;
        return true;
    case SECCOMP_RET_USER_NOTIF:
        if (ret) *ret = -(int64_t) ENOSYS;
        return true;
    case SECCOMP_RET_KILL_THREAD:
    case SECCOMP_RET_KILL_PROCESS:
    default:
        proc_do_exit(-SIGSYS);
    }

    return true;
}
