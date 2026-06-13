#pragma once
#include "mm/vmm.h"
#include <stdint.h>

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFCLASS64 2
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 0x3E

#define PT_LOAD 1
#define PT_INTERP 3
#define PF_X (1 << 0)
#define PF_W (1 << 1)
#define PF_R (1 << 2)

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_ENTRY 9
#define AT_RANDOM 25
#define AT_EXECFN 31

typedef struct {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

typedef struct {
    vmm_space_t* space;
    uint64_t entry;      /* jump target: interpreter entry if PT_INTERP else prog entry */
    uint64_t prog_entry; /* AT_ENTRY: main programs own entry point */
    uint64_t brk;
    uint64_t phdr_va;
    uint16_t phentsize;
    uint16_t phnum;
    uint64_t interp_base; /* AT_BASE: where interpreter was loaded (0 = no interpreter) */
    char interp[256];     /* PT_INTERP path, empty if none */
} elf_load_result_t;

int elf_load_into(vmm_space_t* space, const void* data, uint64_t size, uint64_t bias,
                  elf_load_result_t* out);
int elf_load(const void* data, uint64_t size, elf_load_result_t* out);
