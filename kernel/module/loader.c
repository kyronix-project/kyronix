#include "loader.h"

#include "arch/x86_64/spinlock.h"
#include "fs/vfs_internal.h"
#include "lib/kallsyms.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "syscall/internal.h"
#include "syscall/syscall.h"
#include "version.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MODULE_MAX 32
#define MODULE_MAX_DEPS 32
#define MODULE_NAME_MAX 63
#define MODULE_LICENSE_MAX 31
#define MODULE_IMAGE_MAX (16ULL * 1024 * 1024)
#define MODULE_REGION_START 0xffffffffc0000000ULL
#define MODULE_REGION_END 0xffffffffe0000000ULL

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_REL 1
#define EM_X86_64 62
#define EV_CURRENT 1

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_DYNSYM 11

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHF_TLS 0x400

#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_PLT32 4
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_PC64 24

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
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} __attribute__((packed)) elf64_shdr_t;

typedef struct {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) elf64_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
} __attribute__((packed)) elf64_rela_t;

typedef struct {
    char *name;
    uint64_t addr;
} module_export_t;

typedef enum {
    MODULE_EMPTY = 0,
    MODULE_LOADING,
    MODULE_LIVE,
    MODULE_UNLOADING,
} module_state_t;

typedef struct loaded_module {
    module_state_t state;
    char name[MODULE_NAME_MAX + 1];
    char license[MODULE_LICENSE_MAX + 1];
    uint64_t base;
    uint64_t size;
    uint64_t page_count;
    uint64_t *pages;
    void (*exit)(void);
    module_export_t *exports;
    uint32_t export_count;
    struct loaded_module *deps[MODULE_MAX_DEPS];
    uint32_t dep_count;
    uint32_t users;
} loaded_module_t;

typedef struct {
    const uint8_t *image;
    uint64_t image_size;
    const elf64_ehdr_t *eh;
    uint64_t *sec_addr;
    uint64_t layout_size;
    loaded_module_t *mod;
} load_ctx_t;

static loaded_module_t g_modules[MODULE_MAX];
static spinlock_t g_module_op_lock = SPINLOCK_INIT;
static spinlock_t g_module_list_lock = SPINLOCK_INIT;

static bool range_ok(uint64_t off, uint64_t len, uint64_t total) {
    return off <= total && len <= total - off;
}

static const elf64_shdr_t *section_at(const load_ctx_t *ctx, uint32_t index) {
    if (index >= ctx->eh->e_shnum) return NULL;
    return (const elf64_shdr_t *) (ctx->image + ctx->eh->e_shoff +
                                   (uint64_t) index * ctx->eh->e_shentsize);
}

static const char *table_string(const load_ctx_t *ctx, const elf64_shdr_t *strtab,
                                uint32_t offset) {
    if (!strtab || strtab->sh_type != SHT_STRTAB || offset >= strtab->sh_size) return NULL;
    if (!range_ok(strtab->sh_offset, strtab->sh_size, ctx->image_size)) return NULL;
    const char *s = (const char *) ctx->image + strtab->sh_offset + offset;
    uint64_t remain = strtab->sh_size - offset;
    return strnlen(s, remain) < remain ? s : NULL;
}

static const char *section_name(const load_ctx_t *ctx, const elf64_shdr_t *section) {
    if (ctx->eh->e_shstrndx >= ctx->eh->e_shnum) return NULL;
    return table_string(ctx, section_at(ctx, ctx->eh->e_shstrndx), section->sh_name);
}

static bool module_name_valid(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n > MODULE_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static int copy_modinfo_value(char *out, size_t out_size, const uint8_t *data, uint64_t size,
                              const char *key) {
    size_t key_len = strlen(key);
    for (uint64_t off = 0; off < size;) {
        const char *item = (const char *) data + off;
        size_t item_len = strnlen(item, size - off);
        if (item_len == size - off) return -ENOEXEC;
        if (item_len > key_len && strncmp(item, key, key_len) == 0) {
            size_t value_len = item_len - key_len;
            if (value_len >= out_size) return -EINVAL;
            memcpy(out, item + key_len, value_len);
            out[value_len] = '\0';
            return 1;
        }
        off += item_len + 1;
    }
    return 0;
}

static int parse_modinfo(load_ctx_t *ctx, char *name, char *license) {
    name[0] = '\0';
    license[0] = '\0';
    for (uint32_t i = 0; i < ctx->eh->e_shnum; i++) {
        const elf64_shdr_t *sh = section_at(ctx, i);
        const char *sname = section_name(ctx, sh);
        if (!sname || strcmp(sname, ".modinfo") != 0) continue;
        if (!range_ok(sh->sh_offset, sh->sh_size, ctx->image_size)) return -ENOEXEC;
        const uint8_t *data = ctx->image + sh->sh_offset;
        char vermagic[64];
        int rc = copy_modinfo_value(name, MODULE_NAME_MAX + 1, data, sh->sh_size, "name=");
        if (rc <= 0) return rc < 0 ? rc : -ENOEXEC;
        rc = copy_modinfo_value(license, MODULE_LICENSE_MAX + 1, data, sh->sh_size, "license=");
        if (rc < 0) return rc;
        rc = copy_modinfo_value(vermagic, sizeof(vermagic), data, sh->sh_size, "vermagic=");
        if (rc < 0) return rc;
        if (rc > 0 && strcmp(vermagic, KERNEL_VERSION) != 0) return -ENOEXEC;
        if (!module_name_valid(name)) return -EINVAL;
        if (!license[0]) strcpy(license, "unspecified");
        return 0;
    }
    return -ENOEXEC;
}

static int validate_elf(load_ctx_t *ctx) {
    if (ctx->image_size < sizeof(elf64_ehdr_t)) return -ENOEXEC;
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *) ctx->image;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
        eh->e_ident[3] != 'F' || eh->e_ident[4] != ELFCLASS64 ||
        eh->e_ident[5] != ELFDATA2LSB || eh->e_type != ET_REL ||
        eh->e_machine != EM_X86_64 || eh->e_version != EV_CURRENT ||
        eh->e_ehsize < sizeof(elf64_ehdr_t) || eh->e_shentsize < sizeof(elf64_shdr_t) ||
        !eh->e_shnum)
        return -ENOEXEC;
    uint64_t shbytes = (uint64_t) eh->e_shnum * eh->e_shentsize;
    if (!range_ok(eh->e_shoff, shbytes, ctx->image_size)) return -ENOEXEC;
    if (eh->e_shstrndx >= eh->e_shnum) return -ENOEXEC;
    ctx->eh = eh;
    return 0;
}

static int prepare_layout(load_ctx_t *ctx) {
    ctx->sec_addr = (uint64_t *) kcalloc(ctx->eh->e_shnum, sizeof(uint64_t));
    if (!ctx->sec_addr) return -ENOMEM;
    uint64_t cursor = 0;
    for (uint32_t i = 0; i < ctx->eh->e_shnum; i++) {
        const elf64_shdr_t *sh = section_at(ctx, i);
        if (!(sh->sh_flags & SHF_ALLOC)) continue;
        if (sh->sh_flags & SHF_TLS) return -ENOTSUP;
        if ((sh->sh_flags & (SHF_WRITE | SHF_EXECINSTR)) ==
            (SHF_WRITE | SHF_EXECINSTR))
            return -ENOEXEC;
        if (sh->sh_type != SHT_NOBITS &&
            !range_ok(sh->sh_offset, sh->sh_size, ctx->image_size))
            return -ENOEXEC;
        uint64_t align = sh->sh_addralign ? sh->sh_addralign : 1;
        if ((align & (align - 1)) || align > PAGE_SIZE) return -ENOEXEC;
        cursor = PAGE_ALIGN_UP(cursor);
        if (sh->sh_size > MODULE_IMAGE_MAX || cursor > MODULE_IMAGE_MAX - sh->sh_size)
            return -EFBIG;
        ctx->sec_addr[i] = cursor; // converted to an absolute address after reservation
        cursor += sh->sh_size;
    }
    ctx->layout_size = PAGE_ALIGN_UP(cursor);
    if (!ctx->layout_size || ctx->layout_size > MODULE_IMAGE_MAX) return -EFBIG;
    return 0;
}

static bool ranges_overlap(uint64_t a, uint64_t asz, uint64_t b, uint64_t bsz) {
    return a < b + bsz && b < a + asz;
}

static uint64_t find_module_base(uint64_t size) {
    uint64_t candidate = MODULE_REGION_START;
    for (;;) {
        bool moved = false;
        for (int i = 0; i < MODULE_MAX; i++) {
            loaded_module_t *m = &g_modules[i];
            if (m->state == MODULE_EMPTY) continue;
            if (ranges_overlap(candidate, size, m->base, m->size)) {
                candidate = PAGE_ALIGN_UP(m->base + m->size);
                moved = true;
                break;
            }
        }
        if (!moved) break;
    }
    if (candidate > MODULE_REGION_END || size > MODULE_REGION_END - candidate) return 0;
    return candidate;
}

static loaded_module_t *reserve_module(const char *name, const char *license, uint64_t size,
                                        int *error) {
    spin_lock(&g_module_list_lock);
    loaded_module_t *slot = NULL;
    for (int i = 0; i < MODULE_MAX; i++) {
        if (g_modules[i].state != MODULE_EMPTY && strcmp(g_modules[i].name, name) == 0) {
            spin_unlock(&g_module_list_lock);
            *error = -EEXIST;
            return NULL;
        }
        if (!slot && g_modules[i].state == MODULE_EMPTY) slot = &g_modules[i];
    }
    if (!slot) {
        spin_unlock(&g_module_list_lock);
        *error = -ENOMEM;
        return NULL;
    }
    uint64_t base = find_module_base(size);
    if (!base) {
        spin_unlock(&g_module_list_lock);
        *error = -ENOMEM;
        return NULL;
    }
    memset(slot, 0, sizeof(*slot));
    slot->state = MODULE_LOADING;
    slot->base = base;
    slot->size = size;
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    strncpy(slot->license, license, sizeof(slot->license) - 1);
    spin_unlock(&g_module_list_lock);
    return slot;
}

static void unmap_module_pages(loaded_module_t *mod) {
    if (!mod->pages) return;
    for (uint64_t i = 0; i < mod->page_count; i++) {
        vmm_unmap(&g_kernel_space, mod->base + i * PAGE_SIZE);
        if (mod->pages[i]) pmm_free((void *) mod->pages[i]);
    }
    kfree(mod->pages);
    mod->pages = NULL;
    mod->page_count = 0;
}

static int map_module_pages(loaded_module_t *mod) {
    mod->page_count = mod->size / PAGE_SIZE;
    mod->pages = (uint64_t *) kcalloc(mod->page_count, sizeof(uint64_t));
    if (!mod->pages) return -ENOMEM;
    for (uint64_t i = 0; i < mod->page_count; i++) {
        void *phys = pmm_alloc_zeroed();
        if (!phys) {
            unmap_module_pages(mod);
            return -ENOMEM;
        }
        mod->pages[i] = (uint64_t) phys;
        if (vmm_map(&g_kernel_space, mod->base + i * PAGE_SIZE, (uint64_t) phys, VMM_KDATA) < 0) {
            pmm_free(phys);
            mod->pages[i] = 0;
            unmap_module_pages(mod);
            return -ENOMEM;
        }
    }
    return 0;
}

static int copy_sections(load_ctx_t *ctx) {
    for (uint32_t i = 0; i < ctx->eh->e_shnum; i++) {
        const elf64_shdr_t *sh = section_at(ctx, i);
        if (!(sh->sh_flags & SHF_ALLOC)) continue;
        ctx->sec_addr[i] += ctx->mod->base;
        if (sh->sh_type != SHT_NOBITS && sh->sh_size)
            memcpy((void *) ctx->sec_addr[i], ctx->image + sh->sh_offset, sh->sh_size);
    }
    return 0;
}

static int add_dependency(loaded_module_t *consumer, loaded_module_t *provider) {
    for (uint32_t i = 0; i < consumer->dep_count; i++)
        if (consumer->deps[i] == provider) return 0;
    if (consumer->dep_count >= MODULE_MAX_DEPS) return -EFBIG;
    consumer->deps[consumer->dep_count++] = provider;
    provider->users++;
    return 0;
}

static uint64_t loaded_symbol(load_ctx_t *ctx, const char *name, int *error) {
    uint64_t addr = kallsyms_lookup_name(name);
    if (addr) return addr;
    for (int i = 0; i < MODULE_MAX; i++) {
        loaded_module_t *m = &g_modules[i];
        if (m->state != MODULE_LIVE) continue;
        for (uint32_t j = 0; j < m->export_count; j++) {
            if (strcmp(m->exports[j].name, name) != 0) continue;
            *error = add_dependency(ctx->mod, m);
            return *error ? 0 : m->exports[j].addr;
        }
    }
    return 0;
}

static int symbol_value(load_ctx_t *ctx, const elf64_shdr_t *symtab,
                        const elf64_shdr_t *strtab, uint32_t index, uint64_t *value) {
    if (!symtab->sh_entsize || symtab->sh_entsize < sizeof(elf64_sym_t) ||
        index >= symtab->sh_size / symtab->sh_entsize)
        return -ENOEXEC;
    const elf64_sym_t *sym =
        (const elf64_sym_t *) (ctx->image + symtab->sh_offset +
                               (uint64_t) index * symtab->sh_entsize);
    uint8_t bind = sym->st_info >> 4;
    if (sym->st_shndx == SHN_UNDEF) {
        const char *name = table_string(ctx, strtab, sym->st_name);
        if (!name) return -ENOEXEC;
        int error = 0;
        uint64_t addr = loaded_symbol(ctx, name, &error);
        if (error) return error;
        if (!addr && bind != STB_WEAK) {
            log_error("module %s: unknown symbol %s", ctx->mod->name, name);
            return -ENOEXEC;
        }
        *value = addr;
        return 0;
    }
    if (sym->st_shndx == SHN_ABS) {
        *value = sym->st_value;
        return 0;
    }
    if (sym->st_shndx == SHN_COMMON || sym->st_shndx >= ctx->eh->e_shnum ||
        !ctx->sec_addr[sym->st_shndx])
        return -ENOEXEC;
    const elf64_shdr_t *defined = section_at(ctx, sym->st_shndx);
    if (sym->st_value > defined->sh_size) return -ENOEXEC;
    *value = ctx->sec_addr[sym->st_shndx] + sym->st_value;
    return 0;
}

static void write_u32(void *where, uint32_t value) { memcpy(where, &value, sizeof(value)); }
static void write_u64(void *where, uint64_t value) { memcpy(where, &value, sizeof(value)); }

static int apply_one_relocation(void *where, uint32_t type, uint64_t symbol, int64_t addend,
                                uint64_t place) {
    __int128 value;
    switch (type) {
    case R_X86_64_NONE:
        return 0;
    case R_X86_64_64:
        write_u64(where, symbol + (uint64_t) addend);
        return 0;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
        value = (__int128) symbol + addend - place;
        if (value < INT32_MIN || value > INT32_MAX) return -EOVERFLOW;
        write_u32(where, (uint32_t) (int32_t) value);
        return 0;
    case R_X86_64_32:
        value = (__int128) symbol + addend;
        if (value < 0 || value > UINT32_MAX) return -EOVERFLOW;
        write_u32(where, (uint32_t) value);
        return 0;
    case R_X86_64_32S:
    {
        uint64_t truncated = symbol + (uint64_t) addend;
        uint32_t low = (uint32_t) truncated;
        if ((uint64_t) (int64_t) (int32_t) low != truncated) return -EOVERFLOW;
        write_u32(where, low);
        return 0;
    }
    case R_X86_64_PC64:
        write_u64(where, (uint64_t) ((__int128) symbol + addend - place));
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int apply_relocations(load_ctx_t *ctx) {
    int result = 0;
    spin_lock(&g_module_list_lock);
    for (uint32_t i = 0; i < ctx->eh->e_shnum && result == 0; i++) {
        const elf64_shdr_t *rela = section_at(ctx, i);
        if (rela->sh_type == SHT_REL && rela->sh_info < ctx->eh->e_shnum &&
            (section_at(ctx, rela->sh_info)->sh_flags & SHF_ALLOC)) {
            result = -ENOTSUP;
            break;
        }
        if (rela->sh_type != SHT_RELA) continue;
        if (rela->sh_info >= ctx->eh->e_shnum || rela->sh_link >= ctx->eh->e_shnum ||
            !rela->sh_entsize || rela->sh_entsize < sizeof(elf64_rela_t) ||
            rela->sh_size % rela->sh_entsize ||
            !range_ok(rela->sh_offset, rela->sh_size, ctx->image_size)) {
            result = -ENOEXEC;
            break;
        }
        const elf64_shdr_t *target = section_at(ctx, rela->sh_info);
        const elf64_shdr_t *symtab = section_at(ctx, rela->sh_link);
        if (!(target->sh_flags & SHF_ALLOC)) continue;
        if ((symtab->sh_type != SHT_SYMTAB && symtab->sh_type != SHT_DYNSYM) ||
            symtab->sh_link >= ctx->eh->e_shnum ||
            !range_ok(symtab->sh_offset, symtab->sh_size, ctx->image_size)) {
            result = -ENOEXEC;
            break;
        }
        const elf64_shdr_t *strtab = section_at(ctx, symtab->sh_link);
        for (uint64_t off = 0; off < rela->sh_size; off += rela->sh_entsize) {
            const elf64_rela_t *r =
                (const elf64_rela_t *) (ctx->image + rela->sh_offset + off);
            uint32_t type = (uint32_t) r->r_info;
            uint32_t width = (type == R_X86_64_64 || type == R_X86_64_PC64) ? 8 : 4;
            if (type == R_X86_64_NONE) width = 0;
            if (r->r_offset > target->sh_size || width > target->sh_size - r->r_offset) {
                result = -ENOEXEC;
                break;
            }
            uint64_t symbol;
            result = symbol_value(ctx, symtab, strtab, (uint32_t) (r->r_info >> 32), &symbol);
            if (result < 0) break;
            uint64_t place = ctx->sec_addr[rela->sh_info] + r->r_offset;
            result = apply_one_relocation((void *) place, type, symbol, r->r_addend, place);
            if (result < 0) {
                log_error("module %s: unsupported/overflow relocation %u", ctx->mod->name, type);
                break;
            }
        }
    }
    spin_unlock(&g_module_list_lock);
    return result;
}

static void free_exports(loaded_module_t *mod) {
    for (uint32_t i = 0; i < mod->export_count; i++)
        if (mod->exports[i].name) kfree(mod->exports[i].name);
    if (mod->exports) kfree(mod->exports);
    mod->exports = NULL;
    mod->export_count = 0;
}

static bool exportable_symbol(load_ctx_t *ctx, const elf64_sym_t *sym, const char *name) {
    uint8_t bind = sym->st_info >> 4;
    if ((bind != STB_GLOBAL && bind != STB_WEAK) || !name || !name[0]) return false;
    if (strcmp(name, "init_module") == 0 || strcmp(name, "cleanup_module") == 0) return false;
    if (sym->st_shndx == SHN_ABS) return true;
    if (sym->st_shndx >= ctx->eh->e_shnum || !ctx->sec_addr[sym->st_shndx]) return false;
    return sym->st_value <= section_at(ctx, sym->st_shndx)->sh_size;
}

static int build_exports(load_ctx_t *ctx) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < ctx->eh->e_shnum; i++) {
        const elf64_shdr_t *symtab = section_at(ctx, i);
        if (symtab->sh_type != SHT_SYMTAB || symtab->sh_entsize < sizeof(elf64_sym_t) ||
            symtab->sh_link >= ctx->eh->e_shnum)
            continue;
        const elf64_shdr_t *strtab = section_at(ctx, symtab->sh_link);
        if (!range_ok(symtab->sh_offset, symtab->sh_size, ctx->image_size)) return -ENOEXEC;
        for (uint64_t off = 0; off + sizeof(elf64_sym_t) <= symtab->sh_size;
             off += symtab->sh_entsize) {
            const elf64_sym_t *sym = (const elf64_sym_t *) (ctx->image + symtab->sh_offset + off);
            const char *name = table_string(ctx, strtab, sym->st_name);
            if (exportable_symbol(ctx, sym, name)) count++;
        }
    }
    if (!count) return 0;
    module_export_t *exports = (module_export_t *) kcalloc(count, sizeof(*exports));
    if (!exports) return -ENOMEM;
    uint32_t out = 0;
    int result = 0;
    for (uint32_t i = 0; i < ctx->eh->e_shnum && result == 0; i++) {
        const elf64_shdr_t *symtab = section_at(ctx, i);
        if (symtab->sh_type != SHT_SYMTAB || symtab->sh_entsize < sizeof(elf64_sym_t) ||
            symtab->sh_link >= ctx->eh->e_shnum)
            continue;
        const elf64_shdr_t *strtab = section_at(ctx, symtab->sh_link);
        for (uint64_t off = 0; off + sizeof(elf64_sym_t) <= symtab->sh_size;
             off += symtab->sh_entsize) {
            const elf64_sym_t *sym = (const elf64_sym_t *) (ctx->image + symtab->sh_offset + off);
            const char *name = table_string(ctx, strtab, sym->st_name);
            if (!exportable_symbol(ctx, sym, name)) continue;
            size_t len = strlen(name);
            exports[out].name = (char *) kmalloc(len + 1);
            if (!exports[out].name) {
                result = -ENOMEM;
                break;
            }
            memcpy(exports[out].name, name, len + 1);
            if (sym->st_shndx == SHN_ABS)
                exports[out].addr = sym->st_value;
            else
                exports[out].addr = ctx->sec_addr[sym->st_shndx] + sym->st_value;
            out++;
        }
    }
    if (result < 0) {
        for (uint32_t i = 0; i < out; i++) kfree(exports[i].name);
        kfree(exports);
        return result;
    }

    spin_lock(&g_module_list_lock);
    for (uint32_t i = 0; i < out && result == 0; i++) {
        if (kallsyms_lookup_name(exports[i].name)) {
            result = -EEXIST;
            break;
        }
        for (int m = 0; m < MODULE_MAX && result == 0; m++) {
            if (g_modules[m].state != MODULE_LIVE) continue;
            for (uint32_t j = 0; j < g_modules[m].export_count; j++)
                if (strcmp(exports[i].name, g_modules[m].exports[j].name) == 0) {
                    result = -EEXIST;
                    break;
                }
        }
    }
    if (result == 0) {
        ctx->mod->exports = exports;
        ctx->mod->export_count = out;
    }
    spin_unlock(&g_module_list_lock);
    if (result < 0) {
        for (uint32_t i = 0; i < out; i++) kfree(exports[i].name);
        kfree(exports);
    }
    return result;
}

static int find_entry(load_ctx_t *ctx, const char *wanted, uint64_t *value) {
    for (uint32_t i = 0; i < ctx->eh->e_shnum; i++) {
        const elf64_shdr_t *symtab = section_at(ctx, i);
        if (symtab->sh_type != SHT_SYMTAB || symtab->sh_entsize < sizeof(elf64_sym_t) ||
            symtab->sh_link >= ctx->eh->e_shnum ||
            !range_ok(symtab->sh_offset, symtab->sh_size, ctx->image_size))
            continue;
        const elf64_shdr_t *strtab = section_at(ctx, symtab->sh_link);
        for (uint64_t off = 0; off + sizeof(elf64_sym_t) <= symtab->sh_size;
             off += symtab->sh_entsize) {
            const elf64_sym_t *sym = (const elf64_sym_t *) (ctx->image + symtab->sh_offset + off);
            const char *name = table_string(ctx, strtab, sym->st_name);
            if (!name || strcmp(name, wanted) != 0) continue;
            if (sym->st_shndx == SHN_UNDEF || sym->st_shndx >= ctx->eh->e_shnum ||
                !ctx->sec_addr[sym->st_shndx])
                return -ENOEXEC;
            if (sym->st_value > section_at(ctx, sym->st_shndx)->sh_size) return -ENOEXEC;
            *value = ctx->sec_addr[sym->st_shndx] + sym->st_value;
            return 1;
        }
    }
    return 0;
}

static int protect_sections(load_ctx_t *ctx) {
    for (uint64_t off = 0; off < ctx->mod->size; off += PAGE_SIZE)
        if (vmm_protect(&g_kernel_space, ctx->mod->base + off, VMM_PRESENT | VMM_NX) < 0)
            return -EIO;
    for (uint32_t i = 0; i < ctx->eh->e_shnum; i++) {
        const elf64_shdr_t *sh = section_at(ctx, i);
        if (!(sh->sh_flags & SHF_ALLOC) || !sh->sh_size) continue;
        uint64_t flags = VMM_PRESENT | VMM_NX;
        if (sh->sh_flags & SHF_WRITE)
            flags = VMM_KDATA;
        else if (sh->sh_flags & SHF_EXECINSTR)
            flags = VMM_KCODE;
        uint64_t end = PAGE_ALIGN_UP(ctx->sec_addr[i] + sh->sh_size);
        for (uint64_t page = PAGE_ALIGN_DOWN(ctx->sec_addr[i]); page < end; page += PAGE_SIZE)
            if (vmm_protect(&g_kernel_space, page, flags) < 0) return -EIO;
    }
    __asm__ volatile("mfence" ::: "memory");
    return 0;
}

static void release_dependencies(loaded_module_t *mod) {
    for (uint32_t i = 0; i < mod->dep_count; i++)
        if (mod->deps[i] && mod->deps[i]->users) mod->deps[i]->users--;
    mod->dep_count = 0;
}

static void abandon_module(load_ctx_t *ctx) {
    loaded_module_t *mod = ctx->mod;
    unmap_module_pages(mod);
    free_exports(mod);
    spin_lock(&g_module_list_lock);
    release_dependencies(mod);
    memset(mod, 0, sizeof(*mod));
    spin_unlock(&g_module_list_lock);
}

static int module_load_image(const void *image, uint64_t image_size) {
    load_ctx_t ctx = {
        .image = (const uint8_t *) image,
        .image_size = image_size,
    };
    int result = validate_elf(&ctx);
    if (result < 0) return result;
    char name[MODULE_NAME_MAX + 1], license[MODULE_LICENSE_MAX + 1];
    result = parse_modinfo(&ctx, name, license);
    if (result < 0) return result;
    result = prepare_layout(&ctx);
    if (result < 0) {
        if (ctx.sec_addr) kfree(ctx.sec_addr);
        return result;
    }
    ctx.mod = reserve_module(name, license, ctx.layout_size, &result);
    if (!ctx.mod) {
        kfree(ctx.sec_addr);
        return result;
    }
    result = map_module_pages(ctx.mod);
    if (result < 0) goto fail;
    copy_sections(&ctx);
    result = apply_relocations(&ctx);
    if (result < 0) goto fail;
    result = build_exports(&ctx);
    if (result < 0) goto fail;
    uint64_t init_addr = 0, exit_addr = 0;
    result = find_entry(&ctx, "init_module", &init_addr);
    if (result <= 0) {
        result = result < 0 ? result : -ENOEXEC;
        goto fail;
    }
    result = find_entry(&ctx, "cleanup_module", &exit_addr);
    if (result < 0) goto fail;
    ctx.mod->exit = result > 0 ? (void (*)(void)) exit_addr : NULL;
    result = protect_sections(&ctx);
    if (result < 0) goto fail;

    int init_result = ((int (*)(void)) init_addr)();
    if (init_result != 0) {
        result = init_result < 0 ? init_result : -EINVAL;
        goto fail;
    }
    spin_lock(&g_module_list_lock);
    ctx.mod->state = MODULE_LIVE;
    spin_unlock(&g_module_list_lock);
    log_info("module: loaded %s (%lu bytes, license %s)", ctx.mod->name, ctx.mod->size,
             ctx.mod->license);
    kfree(ctx.sec_addr);
    return 0;

fail:
    log_error("module: failed to load %s (%d)", name, result);
    abandon_module(&ctx);
    kfree(ctx.sec_addr);
    return result;
}

static int validate_user_params(const char *params) {
    if (!params) return 0;
    for (uint64_t i = 0; i < 4096; i++) {
        if (i == 0 || (((uint64_t) (uintptr_t) params + i) & (PAGE_SIZE - 1)) == 0)
            if (!uptr_ok(params + i, 1)) return -EFAULT;
        if (!params[i]) return 0;
    }
    return -E2BIG;
}

static int copy_user_module_name(char *out, const char *user) {
    if (!user) return -EFAULT;
    for (size_t i = 0; i <= MODULE_NAME_MAX; i++) {
        if (i == 0 || (((uint64_t) (uintptr_t) user + i) & (PAGE_SIZE - 1)) == 0)
            if (!uptr_ok(user + i, 1)) return -EFAULT;
        out[i] = user[i];
        if (!out[i]) return module_name_valid(out) ? 0 : -EINVAL;
    }
    out[MODULE_NAME_MAX] = '\0';
    return -ENAMETOOLONG;
}

int64_t sys_init_module(const void *image, uint64_t length, const char *params) {
    if (!host_priv()) return -(int64_t) EPERM;
    if (!image) return -(int64_t) EFAULT;
    if (!length) return -(int64_t) ENOEXEC;
    if (length > MODULE_IMAGE_MAX) return -(int64_t) EFBIG;
    int result = validate_user_params(params);
    if (result < 0) return result;
    if (!uptr_ok(image, length)) return -(int64_t) EFAULT;
    void *copy = kmalloc(length);
    if (!copy) return -(int64_t) ENOMEM;
    memcpy(copy, image, length);
    spin_lock(&g_module_op_lock);
    result = module_load_image(copy, length);
    spin_unlock(&g_module_op_lock);
    kfree(copy);
    return result;
}

int64_t sys_finit_module(int fd, const char *params, uint32_t flags) {
    if (!host_priv()) return -(int64_t) EPERM;
    if (flags) return -(int64_t) EINVAL;
    int result = validate_user_params(params);
    if (result < 0) return result;
    vfs_file_t *file = fd_get_file(fd);
    if (!file) return -(int64_t) EBADF;
    vfs_node_t *node = file->node;
    if (!node || node->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
    if (!node->size) return -(int64_t) ENOEXEC;
    if (node->size > MODULE_IMAGE_MAX) return -(int64_t) EFBIG;
    uint8_t *image = (uint8_t *) kmalloc(node->size);
    if (!image) return -(int64_t) ENOMEM;
    if (node->fs_ops && node->fs_ops->read) {
        int64_t got = node->fs_ops->read(node, (char *) image, 0, node->size);
        if (got < 0 || (uint64_t) got != node->size) {
            kfree(image);
            return got < 0 ? got : -(int64_t) EIO;
        }
    } else if (node->data) {
        memcpy(image, node->data, node->size);
    } else {
        kfree(image);
        return -(int64_t) EIO;
    }
    spin_lock(&g_module_op_lock);
    result = module_load_image(image, node->size);
    spin_unlock(&g_module_op_lock);
    kfree(image);
    return result;
}

int module_load_path(const char *path) {
    if (!path) return -EINVAL;
    vfs_node_t *node = vfs_lookup(path);
    if (!node) return -ENOENT;
    int result;
    if (node->type != VFS_TYPE_REG)
        result = -EINVAL;
    else if (!node->size)
        result = -ENOEXEC;
    else if (node->size > MODULE_IMAGE_MAX)
        result = -EFBIG;
    else {
        uint8_t *image = (uint8_t *) kmalloc(node->size);
        if (!image) {
            result = -ENOMEM;
        } else {
            if (node->fs_ops && node->fs_ops->read) {
                int64_t got = node->fs_ops->read(node, (char *) image, 0, node->size);
                result = (got < 0) ? (int) got : ((uint64_t) got == node->size ? 0 : -EIO);
            } else if (node->data) {
                memcpy(image, node->data, node->size);
                result = 0;
            } else {
                result = -EIO;
            }
            if (result == 0) {
                spin_lock(&g_module_op_lock);
                result = module_load_image(image, node->size);
                spin_unlock(&g_module_op_lock);
            }
            kfree(image);
        }
    }
    vfs_node_unref_internal(node);
    return result;
}

int64_t sys_delete_module(const char *user_name, uint32_t flags) {
    if (!host_priv()) return -(int64_t) EPERM;
    if (flags) return -(int64_t) EINVAL;
    char name[MODULE_NAME_MAX + 1];
    int result = copy_user_module_name(name, user_name);
    if (result < 0) return result;

    spin_lock(&g_module_op_lock);
    spin_lock(&g_module_list_lock);
    loaded_module_t *mod = NULL;
    for (int i = 0; i < MODULE_MAX; i++)
        if (g_modules[i].state == MODULE_LIVE && strcmp(g_modules[i].name, name) == 0) {
            mod = &g_modules[i];
            break;
        }
    if (!mod) {
        spin_unlock(&g_module_list_lock);
        spin_unlock(&g_module_op_lock);
        return -(int64_t) ENOENT;
    }
    if (mod->users) {
        spin_unlock(&g_module_list_lock);
        spin_unlock(&g_module_op_lock);
        return -(int64_t) EBUSY;
    }
    mod->state = MODULE_UNLOADING;
    void (*exit_fn)(void) = mod->exit;
    spin_unlock(&g_module_list_lock);

    if (exit_fn) exit_fn();
    log_info("module: unloaded %s", mod->name);
    unmap_module_pages(mod);
    free_exports(mod);
    spin_lock(&g_module_list_lock);
    release_dependencies(mod);
    memset(mod, 0, sizeof(*mod));
    spin_unlock(&g_module_list_lock);
    spin_unlock(&g_module_op_lock);
    return 0;
}

static void proc_append(char *out, size_t capacity, size_t *used, const char *fmt,
                        const char *a, uint64_t b, uint32_t c, const char *d, uint64_t e) {
    if (*used >= capacity) return;
    int n = snprintf(out + *used, capacity - *used, fmt, a, b, c, d, e);
    if (n < 0) return;
    if ((size_t) n >= capacity - *used)
        *used = capacity;
    else
        *used += (size_t) n;
}

int64_t module_proc_read(vfs_node_t *node, char *buf, uint64_t len, uint64_t off) {
    (void) node;
    if (!len) return 0;
    const size_t capacity = 8192;
    char *text = (char *) kmalloc(capacity);
    if (!text) return -(int64_t) ENOMEM;
    size_t used = 0;
    //  https://genius.com/9mice-snippet-26032026-lyrics
    //  Я в Маке, я не про бургер
    //  модули делаются в этой ссссссссссссссссссссс
    spin_lock(&g_module_list_lock);
    for (int i = 0; i < MODULE_MAX; i++) {
        loaded_module_t *m = &g_modules[i];
        if (m->state != MODULE_LIVE) continue;
        char users[512];
        size_t upos = 0;
        users[0] = '-';
        users[1] = '\0';
        for (int j = 0; j < MODULE_MAX; j++) {
            loaded_module_t *consumer = &g_modules[j];
            if (consumer->state != MODULE_LIVE) continue;
            for (uint32_t k = 0; k < consumer->dep_count; k++) {
                if (consumer->deps[k] != m) continue;
                size_t n = strlen(consumer->name);
                if (upos == 0) users[0] = '\0';
                if (upos && upos + 1 < sizeof(users)) users[upos++] = ',';
                if (upos + n < sizeof(users)) {
                    memcpy(users + upos, consumer->name, n);
                    upos += n;
                    users[upos] = '\0';
                }
            }
        }
        proc_append(text, capacity, &used, "%s %lu %u %s Live 0x%lx\n", m->name, m->size,
                    m->users, users, m->base);
    }
    spin_unlock(&g_module_list_lock);
    if (off >= used) {
        kfree(text);
        return 0;
    }
    uint64_t count = len < used - off ? len : used - off;
    memcpy(buf, text + off, count);
    kfree(text);
    return (int64_t) count;
}
