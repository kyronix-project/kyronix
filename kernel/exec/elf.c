#include "elf.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vma.h"
#include "mm/vmm.h"

#define PIE_BASE 0x400000ULL
#define INTERP_BASE 0x7f0000000000ULL

static int elf_valid(const Elf64_Ehdr *eh, uint64_t size) {
    if (size < sizeof(Elf64_Ehdr)) return 0;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3)
        return 0;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) return 0;
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) return 0;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return 0;
    if (eh->e_machine != EM_X86_64) return 0;
    if (eh->e_version != 1 || eh->e_ehsize < sizeof(Elf64_Ehdr)) return 0;
    if (eh->e_phentsize < sizeof(Elf64_Phdr) || eh->e_phnum == 0) return 0;
    return 1;
}

int elf_load_into(vmm_space_t *space, const void *data, uint64_t size, uint64_t bias,
                  elf_load_result_t *out) {
    if (!space || !out || !data || size < sizeof(Elf64_Ehdr)) return -1;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *) data;
    if (!elf_valid(eh, size)) return -1;

    out->interp[0] = '\0';
    uint64_t brk = 0, phdr_va = 0;
    bool have_load = false, entry_executable = false;

    // the phdr array itself is attacker controlled: bound it against the file
    uint64_t ph_total = (uint64_t) eh->e_phnum * eh->e_phentsize;
    if (eh->e_phoff > size || ph_total > size - eh->e_phoff) return -1;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *) ((const uint8_t *) data + eh->e_phoff +
                                                     (uint64_t) i * eh->e_phentsize);

        if (ph->p_type == PT_INTERP && ph->p_filesz > 0 && ph->p_filesz < 255) {
            /* p_offset is from the file: validate the source range first. */
            if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset) return -1;
            memcpy(out->interp, (const uint8_t *) data + ph->p_offset, ph->p_filesz);
            out->interp[ph->p_filesz] = '\0';
            if (out->interp[ph->p_filesz - 1] != '\0') return -1;
            size_t n = strlen(out->interp);
            while (n > 0 && (unsigned char) out->interp[n - 1] < ' ') out->interp[--n] = '\0';
        }

        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        // overflow-safe source bounds, and file part must fit the mem part
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset) return -1;
        if (ph->p_filesz > ph->p_memsz) return -1;
        if ((ph->p_flags & (PF_W | PF_X)) == (PF_W | PF_X)) return -1;
        if (ph->p_align > 1 &&
            ((ph->p_align & (ph->p_align - 1)) ||
             (ph->p_vaddr & (ph->p_align - 1)) != (ph->p_offset & (ph->p_align - 1))))
            return -1;

        uint64_t vflags = VMM_PRESENT | VMM_USER;
        if (ph->p_flags & PF_W) vflags |= VMM_WRITE | VMM_NX;
        if (!(ph->p_flags & PF_X)) vflags |= VMM_NX;

        uint64_t vaddr = bias + ph->p_vaddr;
        // reject wrap and any segment that would touch the kernel half
        if (vaddr < bias) return -1;                     // bias + p_vaddr overflow
        if (vaddr + ph->p_memsz < vaddr) return -1;      // vaddr + memsz overflow
        if (vaddr + ph->p_memsz > USER_LIMIT) return -1; // maps into kernel half
        have_load = true;
        uint64_t entry = bias + eh->e_entry;
        if ((ph->p_flags & PF_X) && entry >= vaddr && entry - vaddr < ph->p_memsz)
            entry_executable = true;
        if (!phdr_va && eh->e_phoff >= ph->p_offset &&
            eh->e_phoff - ph->p_offset < ph->p_filesz)
            phdr_va = vaddr + (eh->e_phoff - ph->p_offset);
        uint64_t page_base = PAGE_ALIGN_DOWN(vaddr);
        uint64_t page_end = PAGE_ALIGN_UP(vaddr + ph->p_memsz);
        if (vma_conflicts(space, page_base, page_end - page_base)) return -1;

        for (uint64_t pg = page_base; pg < page_end; pg += PAGE_SIZE) {
            void *phys = pmm_alloc_zeroed();
            if (!phys) return -1;
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
                memcpy((uint8_t *) phys_to_virt((uint64_t) phys) + doff,
                       (const uint8_t *) data + soff, ce - cs);
            }
        }
        uint32_t prot = 0;
        if (ph->p_flags & PF_R) prot |= PROT_READ;
        if (ph->p_flags & PF_W) prot |= PROT_WRITE;
        if (ph->p_flags & PF_X) prot |= PROT_EXEC;
        if (vma_add(space, page_base, page_end - page_base, prot, 0, true) < 0)
            return -1;

        uint64_t end = PAGE_ALIGN_UP(vaddr + ph->p_memsz);
        if (end > brk) brk = end;
    }

    out->prog_entry = bias + eh->e_entry;
    if (out->prog_entry < bias || out->prog_entry >= USER_LIMIT) return -1;
    if (!have_load || !entry_executable) return -1;
    out->phdr_va = phdr_va;
    out->phentsize = eh->e_phentsize;
    out->phnum = eh->e_phnum;
    out->brk = brk;
    return 0;
}

int elf_load(const void *data, uint64_t size, elf_load_result_t *out) {
    if (!data || size < sizeof(Elf64_Ehdr)) { return -1; }
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *) data;
    if (!elf_valid(eh, size)) { return -1; }

    vmm_space_t *space = vmm_space_new();
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
