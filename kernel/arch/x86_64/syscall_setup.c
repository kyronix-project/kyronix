#include "syscall_setup.h"
#include "cpu.h"
#include "gdt.h"
#include "lib/log.h"

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084
#define MSR_KERNEL_GS_BASE 0xC0000102

typedef struct {
    uint64_t kernel_rsp; /* offset 0 */
    uint64_t user_rsp;   /* offset 8 */
} cpu_local_t;

static cpu_local_t g_cpu_local __attribute__((aligned(64)));

#define KERNEL_STACK_PAGES 4
static uint8_t g_syscall_stack[KERNEL_STACK_PAGES * 4096] __attribute__((aligned(16)));

extern void syscall_entry(void);

void cpu_enable_sse(void)
{
    uint64_t cr0 = read_cr0();
    cr0 = (cr0 & ~(1ULL << 2)) | (1ULL << 1);
    write_cr0(cr0);
    write_cr4(read_cr4() | (1ULL << 9) | (1ULL << 10));
}

void syscall_init(void)
{
    cpu_enable_sse();

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1ULL);
    /* STAR[47:32]=0x0008 -> SYSCALL CS=0x08; STAR[63:48]=0x0010 -> SYSRETQ CS=0x20 SS=0x18 */
    wrmsr(MSR_STAR, (0x0010ULL << 48) | (0x0008ULL << 32));
    wrmsr(MSR_LSTAR, (uint64_t) syscall_entry);
    /* clear IF/TF/DF/AC on SYSCALL entry */
    wrmsr(MSR_SFMASK, (1ULL << 9) | (1ULL << 8) | (1ULL << 10) | (1ULL << 18));

    uint64_t rsp0 = (uint64_t) (g_syscall_stack + sizeof(g_syscall_stack));
    g_cpu_local.kernel_rsp = rsp0;
    g_cpu_local.user_rsp = 0;

    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t) &g_cpu_local);
    gdt_set_kernel_stack(rsp0);

    log_info("SYSCALL: LSTAR=0x%016lx  stack=0x%016lx", (uint64_t) syscall_entry, rsp0);
}

void cpu_set_kernel_stack(uint64_t rsp)
{
    g_cpu_local.kernel_rsp = rsp;
    gdt_set_kernel_stack(rsp);
}
