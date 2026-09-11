#ifndef KERNEL_INIT_MB2_H
#define KERNEL_INIT_MB2_H
#include <stdint.h>
#define MB2_BOOTLOADER_MAGIC 0x36D76289u
#define MB2_TAG_END 0
#define MB2_TAG_CMDLINE 1
#define MB2_TAG_BOOT_LOADER_NAME 2
#define MB2_TAG_MODULE 3
#define MB2_TAG_BASIC_MEMINFO 4
#define MB2_TAG_BOOTDEV 5
#define MB2_TAG_MMAP 6
#define MB2_TAG_VBE 7
#define MB2_TAG_FRAMEBUFFER 8
#define MB2_TAG_ELF_SECTIONS 9
#define MB2_TAG_APM 10
#define MB2_TAG_EFI32 11
#define MB2_TAG_EFI64 12
#define MB2_TAG_SMBIOS 13
#define MB2_TAG_ACPI_OLD 14
#define MB2_TAG_ACPI_NEW 15
#define MB2_TAG_NETWORK 16
#define MB2_TAG_EFI_MMAP 17
#define MB2_TAG_EFI_BS 18
#define MB2_TAG_EFI32_IH 19
#define MB2_TAG_EFI64_IH 20
#define MB2_TAG_LOAD_BASE 21
#define MB2_MMAP_AVAILABLE 1
#define MB2_MMAP_RESERVED 2
#define MB2_MMAP_ACPI_RECLAIM 3
#define MB2_MMAP_ACPI_NVS 4
#define MB2_MMAP_BAD 5
#define MB2_FB_TYPE_INDEXED 0
#define MB2_FB_TYPE_RGB 1
#define MB2_FB_TYPE_EGA 2
#define MB2_MAX_MMAP_ENTRIES 64
struct mb2_tag {
    uint32_t type;
    uint32_t size;
};
struct mb2_tag_string {
    uint32_t type;
    uint32_t size;
    char string[1];
};
struct mb2_tag_basic_meminfo {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
};
struct mb2_tag_bootdev {
    uint32_t type;
    uint32_t size;
    uint32_t biosdev;
    uint32_t partition;
    uint32_t sub_partition;
};
struct mb2_mmap_entry {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t zero;
};
struct mb2_tag_mmap {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    struct mb2_mmap_entry entries[1];
};
struct mb2_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
    uint8_t color_info[6];
    uint8_t pad[2];
};
struct mb2_tag_acpi_old {
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[20];
    uint8_t pad[4];
};
struct mb2_tag_acpi_new {
    uint32_t type;
    uint32_t size;
    uint8_t rsdp[36];
};
struct mb2_tag_load_base {
    uint32_t type;
    uint32_t size;
    uint32_t load_base_addr;
};
struct mb2_info {
    int valid;
    uint32_t total_size;
    const char *cmdline;
    const char *boot_loader_name;
    uint32_t mem_lower;
    uint32_t mem_upper;
    int has_bootdev;
    uint32_t biosdev;
    uint32_t partition;
    uint32_t sub_partition;
    int has_load_base;
    uint32_t load_base_addr;
    int has_mmap;
    uint32_t mmap_count;
    struct mb2_mmap_entry mmap[MB2_MAX_MMAP_ENTRIES];
    int has_framebuffer;
    struct mb2_tag_framebuffer framebuffer;
    int has_rsdp_old;
    uint8_t rsdp_old[20];
    int has_rsdp_new;
    uint8_t rsdp_new[36];
    int module_count;
    uint32_t tag_count;
};
void mb2_init(uint32_t magic, const void *mbi);
const struct mb2_info *mb2_get(void);
uint64_t mb2_mem_top(void);
uint32_t mb2_mem_upper_kb(void);
void mb2_dump(void);
#endif