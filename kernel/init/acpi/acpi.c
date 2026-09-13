#include "kernel/init/acpi/acpi.h"
#include "kernel/asm_func.h"
#include "lib/str/str.h"
#include "kernel/mm/pool/pool.h"
#include "drivers/char/console/io.h"

struct ACPI_RSDP *RSDP = NULL;
struct ACPI_XSDT *XSDT = NULL;
struct ACPI_FADT *FADT = NULL;

#define ACPI_TABLES_VADDR 0x60000000u
#define ACPI_SHARED_PD 0x96000u
#define ACPI_PD_INDEX 256
#define ACPI_MAP_PAGE 0x200000u
#define ACPI_MAP_MAX 128

static uint32_t acpi_mapped_phys[ACPI_MAP_MAX];
static uint32_t acpi_map_nr;

static uintptr_t acpi_va_of(uint32_t phys) {
    uint32_t base = phys & ~(ACPI_MAP_PAGE - 1);
    for (uint32_t i = 0; i < acpi_map_nr; i++) {
        if (acpi_mapped_phys[i] == base)
            return ACPI_TABLES_VADDR + i * ACPI_MAP_PAGE + (phys - base);
    }
    return 0;
}

static void *acpi_map(uint32_t phys, uint32_t size) {
    uint32_t base = phys & ~(ACPI_MAP_PAGE - 1);
    uint32_t last = (phys + size - 1) & ~(ACPI_MAP_PAGE - 1);
    for (;;) {
        if (acpi_va_of(base) == 0) {
            if (acpi_map_nr >= ACPI_MAP_MAX)
                return NULL;

            uint64_t *pd = phys_to_virt(ACPI_SHARED_PD);
            pd[ACPI_PD_INDEX + acpi_map_nr] = (uint64_t)base | 0x83;
            __asm__ volatile(
                "invlpg (%0)"
                :
                : "r"(ACPI_TABLES_VADDR + acpi_map_nr * ACPI_MAP_PAGE)
                : "memory");
            acpi_mapped_phys[acpi_map_nr] = base;
            acpi_map_nr++;
        }
        if (base == last)
            break;
        base += ACPI_MAP_PAGE;
    }
    return (void *)acpi_va_of(phys);
}

static void *acpi_map_table(uint64_t phys) {
    struct ACPI_SDT_HEADER *hdr = (struct ACPI_SDT_HEADER *)acpi_map(
        (uint32_t)phys, sizeof(struct ACPI_SDT_HEADER));
    if (!hdr)
        return NULL;
    return acpi_map((uint32_t)phys, hdr->len);
}

uint8_t acpi_checksum(uint8_t *addr, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum += addr[i];
    return (sum == 0);
}

struct ACPI_RSDP *acpi_find_rsdp(void) {
    void *ebda = acpi_map(0x000E0000, 0x20000);
    if (!ebda)
        return NULL;

    for (uintptr_t off = 0; off < 0x20000; off += 16) {
        struct ACPI_RSDP *rsdp = (struct ACPI_RSDP *)((uint8_t *)ebda + off);
        if (memcmp(rsdp->signature, "RSD PTR ", 8) == 0) {
            uint32_t len = (rsdp->revision == 0) ? 20 : rsdp->len;
            if (acpi_checksum((unsigned char *)rsdp, len)) {
                return rsdp;
            }
        }
    }
    return NULL;
}

struct ACPI_SDT_HEADER *acpi_find_table(const char *signature) {
    if (!XSDT) {
        if (RSDP && RSDP->revision >= 2 && RSDP->xsdt_address) {
            XSDT = (struct ACPI_XSDT *)acpi_map_table(RSDP->xsdt_address);
        } else if (RSDP && RSDP->rsdt_address) {
            struct ACPI_RSDT *rsdt =
                (struct ACPI_RSDT *)acpi_map_table(RSDP->rsdt_address);
            if (!rsdt)
                return NULL;
            uint32_t num_entries =
                (rsdt->header.len - sizeof(rsdt->header)) / 4;
            for (uint32_t i = 0; i < num_entries; i++) {
                struct ACPI_SDT_HEADER *hdr =
                    (struct ACPI_SDT_HEADER *)acpi_map_table(rsdt->entry[i]);
                if (hdr && memcmp(hdr->signature, signature, 4) == 0)
                    return hdr;
            }
            return NULL;
        } else {
            return NULL;
        }
    }
    if (!XSDT)
        return NULL;
    if (!acpi_checksum((unsigned char *)XSDT, XSDT->header.len))
        return NULL;

    uint32_t num_entries = (XSDT->header.len - sizeof(XSDT->header)) / 8;
    for (uint32_t i = 0; i < num_entries; i++) {
        struct ACPI_SDT_HEADER *hdr =
            (struct ACPI_SDT_HEADER *)acpi_map_table(XSDT->entry[i]);
        if (hdr && memcmp(hdr->signature, signature, 4) == 0)
            return hdr;
    }
    return NULL;
}

void acpi_init(void) {
    kprintf("[ACPI] searching for RSDP...\n");
    RSDP = acpi_find_rsdp();
    if (!RSDP) {
        kprintf("[ACPI] RSDP not found\n");
        return;
    }
    kprintf("[ACPI] RSDP found, revision %d\n", RSDP, RSDP->revision);

    if (RSDP->revision >= 2 && RSDP->xsdt_address) {
        kprintf("[ACPI] mapping XSDT at 0x%lx\n", RSDP->xsdt_address);
        XSDT = (struct ACPI_XSDT *)acpi_map_table(RSDP->xsdt_address);
        if (!XSDT || !acpi_checksum((uint8_t *)XSDT, XSDT->header.len)) {
            kprintf("[ACPI] XSDT invalid\n");
            return;
        }
        kprintf("[ACPI] XSDT found, %lu entries\n",
                (XSDT->header.len - sizeof(struct ACPI_SDT_HEADER)) / 8);
    } else if (RSDP->rsdt_address) {
        kprintf("[ACPI] mapping RSDT at 0x%x\n", RSDP->rsdt_address);
        struct ACPI_RSDT *rsdt =
            (struct ACPI_RSDT *)acpi_map_table(RSDP->rsdt_address);
        if (!rsdt || !acpi_checksum((uint8_t *)rsdt, rsdt->header.len)) {
            kprintf("[ACPI] RSDT invalid\n");
            return;
        }
        kprintf("[ACPI] RSDT found\n");
    }

    FADT = (struct ACPI_FADT *)acpi_find_table("FACP");
    if (!FADT || !acpi_checksum((uint8_t *)FADT, FADT->header.len)) {
        kprintf("[ACPI] FADT not found or invalid\n");
        return;
    }
    kprintf("[ACPI] FADT found, pm1a=0x%x, smi_cmd=0x%x, acpi_enable=0x%x\n",
            FADT->pm1a_control_block, FADT->smi_command_port,
            FADT->acpi_enable);

    if (!(inw(FADT->pm1a_control_block) & 1)) {
        kprintf("[ACPI] ACPI not enabled, enabling...\n");
        if (FADT->smi_command_port && FADT->acpi_enable) {
            outb(FADT->smi_command_port, FADT->acpi_enable);

            for (int i = 0; i < 300; i++) {
                if (inw(FADT->pm1a_control_block) & 1) {
                    kprintf("[ACPI] ACPI enabled after %d tries\n", i);
                    break;
                }
                for (int j = 0; j < 1000000; j++)
                    asm volatile("pause");
            }
        } else {
            kprintf("[ACPI] smi_cmd=0x%x acpi_enable=0x%x, cannot enable\n",
                    FADT->smi_command_port, FADT->acpi_enable);
        }
    } else {
        kprintf("[ACPI] ACPI already enabled\n");
    }
}

static int acpi_get_s5_slp_typ(uint8_t **slp_typa) {
    struct ACPI_SDT_HEADER *dsdt = acpi_find_table("DSDT");
    if (!dsdt) {
        kprintf("[ACPI] DSDT not found for S5 lookup\n");
        return -1;
    }

    if (!acpi_checksum((uint8_t *)dsdt, dsdt->len)) {
        kprintf("[ACPI] DSDT checksum invalid\n");
        return -1;
    }

    uint8_t *aml = (uint8_t *)dsdt + sizeof(struct ACPI_SDT_HEADER);
    uint32_t aml_len = dsdt->len - sizeof(struct ACPI_SDT_HEADER);
    kprintf("[ACPI] DSDT AML length=%u, scanning for S5 package...\n", aml_len);

    for (uint32_t i = 0; i < aml_len - 3; i++) {
        if (aml[i] == 0x12 && aml[i + 1] == 0x02) {
            uint8_t slp_typ_val = aml[i + 2];
            uint8_t typ_val = aml[i + 3];

            if ((slp_typ_val <= 5) && (typ_val <= 1)) {
                kprintf("[ACPI] Found potential S5 package at offset %u: "
                        "slp_typ=%u, typ=%u\n",
                        i, slp_typ_val, typ_val);
                *slp_typa = &aml[i + 2];
                return 0;
            }
        }
    }

    for (uint32_t i = 0; i < aml_len - 4; i++) {
        if (memcmp(aml + i, "_S5_", 4) == 0) {
            kprintf("[ACPI] Found raw _S5_ at offset %u\n", i);
            uint8_t *p = aml + i + 4;
            for (uint32_t j = 0; j < 128 && p < aml + aml_len - 2; j++, p++) {
                if (*p == 0x12 && *(p + 1) == 0x02) {
                    uint8_t slp_typ_val = *(p + 2);
                    uint8_t typ_val = *(p + 3);
                    kprintf("[ACPI] Parsed S5 package: slp_typ=%u, typ=%u\n",
                            slp_typ_val, typ_val);
                    *slp_typa = p + 2;
                    return 0;
                }
            }
        }
    }

    kprintf("[ACPI] _S5_ not found in DSDT\n");
    return -1;
}

void acpi_shutdown(void) {
    kprintf("[ACPI] shutdown: FADT=%p\n", FADT);
    if (!FADT) {
        kprintf("[ACPI] shutdown aborted: FADT is NULL\n");
        return;
    }

    uint8_t *slp_typa;
    if (acpi_get_s5_slp_typ(&slp_typa) != 0) {
        uint16_t pm1a_cmd = (0 << 10) | (1 << 13);
        outw(FADT->pm1a_control_block, pm1a_cmd);
    } else {
        uint16_t pm1a_cmd = (*slp_typa << 10) | (1 << 13);
        outw(FADT->pm1a_control_block, pm1a_cmd);
    }

    while (1) {
        asm volatile("hlt");
    }
}
