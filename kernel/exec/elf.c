#include "elf.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#define PIE_BASE 0x400000ULL
#define INTERP_BASE 0x7f0000000000ULL

static int elf_valid(const Elf64_Ehdr* eh, uint64_t size)
{
    if (size < sizeof(Elf64_Ehdr))
        return 0;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3)
        return 0;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64)
        return 0;
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB)
        return 0;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN)
        return 0;
    if (eh->e_machine != EM_X86_64)
        return 0;
    if (eh->e_phentsize < sizeof(Elf64_Phdr) || eh->e_phnum == 0)
        return 0;
    return 1;
}

int elf_load_into(vmm_space_t* space, const void* data, uint64_t size, uint64_t bias,
                  elf_load_result_t* out)
{
    if (!data || size < sizeof(Elf64_Ehdr))
        return -1;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*) data;
    if (!elf_valid(eh, size))
        return -1;

    out->interp[0] = '\0';
    uint64_t brk = 0, phdr_va = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*) ((const uint8_t*) data + eh->e_phoff +
                                                    (uint64_t) i * eh->e_phentsize);

        if (ph->p_type == PT_INTERP && ph->p_filesz > 0 && ph->p_filesz < 255) {
            memcpy(out->interp, (const uint8_t*) data + ph->p_offset, ph->p_filesz);
            out->interp[ph->p_filesz] = '\0';
            size_t n = strlen(out->interp);
            while (n > 0 && (unsigned char) out->interp[n - 1] < ' ')
                out->interp[--n] = '\0';
        }

        if (ph->p_type == PT_LOAD && !phdr_va) {
            if (eh->e_phoff >= ph->p_offset && eh->e_phoff < ph->p_offset + ph->p_filesz)
                phdr_va = bias + ph->p_vaddr + (eh->e_phoff - ph->p_offset);
        }

        if (ph->p_type != PT_LOAD || !ph->p_memsz)
            continue;
        if (ph->p_offset + ph->p_filesz > size)
            return -1;

        uint64_t vflags = VMM_PRESENT | VMM_USER;
        if (ph->p_flags & PF_W)
            vflags |= VMM_WRITE | VMM_NX;
        if (!(ph->p_flags & PF_X))
            vflags |= VMM_NX;

        uint64_t vaddr = bias + ph->p_vaddr;
        uint64_t page_base = PAGE_ALIGN_DOWN(vaddr);
        uint64_t page_end = PAGE_ALIGN_UP(vaddr + ph->p_memsz);

        for (uint64_t pg = page_base; pg < page_end; pg += PAGE_SIZE) {
            void* phys = pmm_alloc_zeroed();
            if (!phys)
                return -1;
            if (vmm_map(space, pg, (uint64_t) phys, vflags) < 0) {
                pmm_free(phys);
                return -1;
            }
            uint64_t fs = vaddr, fe = vaddr + ph->p_filesz;
            uint64_t cs = pg > fs ? pg : fs;
            uint64_t ce = pg + PAGE_SIZE < fe ? pg + PAGE_SIZE : fe;
            if (cs < ce) {
                uint64_t doff = cs - pg;
                uint64_t soff = ph->p_offset + (cs - fs);
                memcpy((uint8_t*) phys_to_virt((uint64_t) phys) + doff,
                       (const uint8_t*) data + soff, ce - cs);
            }
        }
        uint64_t end = PAGE_ALIGN_UP(vaddr + ph->p_memsz);
        if (end > brk)
            brk = end;
    }

    out->prog_entry = bias + eh->e_entry;
    out->phdr_va = phdr_va;
    out->phentsize = eh->e_phentsize;
    out->phnum = eh->e_phnum;
    out->brk = brk;
    return 0;
}

int elf_load(const void* data, uint64_t size, elf_load_result_t* out)
{
    if (!data || size < sizeof(Elf64_Ehdr)) {
        log_error("ELF: too small");
        return -1;
    }
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*) data;
    if (!elf_valid(eh, size)) {
        log_error("ELF: invalid header");
        return -1;
    }

    vmm_space_t* space = vmm_space_new();
    if (!space) {
        log_error("ELF: OOM (space)");
        return -1;
    }

    uint64_t bias = (eh->e_type == ET_DYN) ? PIE_BASE : 0;
    memset(out, 0, sizeof(*out));
    out->space = space;

    if (elf_load_into(space, data, size, bias, out) < 0) {
        log_error("ELF: load failed");
        vmm_space_free(space);
        return -1;
    }

    out->entry = out->prog_entry;
    out->interp_base = 0;
    log_info("ELF: loaded entry=0x%lx brk=0x%lx phdr=0x%lx interp=%s", out->prog_entry, out->brk,
             out->phdr_va, out->interp[0] ? out->interp : "(none)");
    return 0;
}
