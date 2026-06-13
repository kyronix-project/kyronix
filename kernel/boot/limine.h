#pragma once

#include <stdint.h>

#define LIMINE_COMMON_MAGIC 0xc7b1dd30df4c8b88, 0x0a82e883a194f07b

#define LIMINE_BASE_REVISION(N)                                                                    \
    __attribute__((used,                                                                           \
                   section(".limine_requests"))) volatile uint64_t __limine_base_revision[3] = {   \
        0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, (N)}

#define LIMINE_BASE_REVISION_SUPPORTED (__limine_base_revision[2] == 0)

#define LIMINE_REQUESTS_START_MARKER                                                               \
    __attribute__((                                                                                \
        used, section(".limine_requests_start"))) volatile uint64_t __limine_requests_start[2] = { \
        0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf}

#define LIMINE_REQUESTS_END_MARKER                                                                 \
    __attribute__((                                                                                \
        used, section(".limine_requests_end"))) volatile uint64_t __limine_requests_end[2] = {     \
        0x1d9310c2c67ea3d2, 0x9d11e0ba4c28b78c}

#define LIMINE_FRAMEBUFFER_REQUEST {LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75, 0xa3148604f6fab11b}

#define LIMINE_FRAMEBUFFER_RGB 1

struct limine_framebuffer {
    void* address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
    uint8_t unused[7];
    uint64_t edid_size;
    void* edid;
    uint64_t mode_count;
    void** modes;
};

struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    struct limine_framebuffer** framebuffers;
};

struct limine_framebuffer_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_framebuffer_response* response;
};

#define LIMINE_MEMMAP_REQUEST {LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62}

#define LIMINE_MEMMAP_USABLE 0
#define LIMINE_MEMMAP_RESERVED 1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE 2
#define LIMINE_MEMMAP_ACPI_NVS 3
#define LIMINE_MEMMAP_BAD_MEMORY 4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES 6
#define LIMINE_MEMMAP_FRAMEBUFFER 7

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_memmap_entry** entries;
};

struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_memmap_response* response;
};

#define LIMINE_HHDM_REQUEST {LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852, 0x63984e959a98244b}

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_hhdm_response* response;
};

#define LIMINE_MODULE_REQUEST {LIMINE_COMMON_MAGIC, 0x3e7e279702be32af, 0xca1c4f3bd1280cee}

struct limine_file {
    uint64_t revision;
    void* address;
    uint64_t size;
    char* path;
    char* cmdline;
    uint32_t media_type;
    uint32_t unused;
    uint32_t tftp_ip;
    uint32_t tftp_port;
    uint32_t partition_index;
    uint32_t mbr_disk_id;
};

struct limine_module_response {
    uint64_t revision;
    uint64_t module_count;
    struct limine_file** modules;
};

struct limine_module_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_module_response* response;
    uint64_t internal_module_count;
    void** internal_modules;
};

// kernel address

#define LIMINE_KERNEL_ADDRESS_REQUEST {LIMINE_COMMON_MAGIC, 0x71ba76863cc55f63, 0xb2644a48c516a487}

struct limine_kernel_address_response {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};

struct limine_kernel_address_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_kernel_address_response* response;
};
