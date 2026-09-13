#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

struct ACPI_RSDP {
    char signature[8];
    uint8_t checksum;
    char oemid[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t len;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct ACPI_SDT_HEADER {
    char signature[4];
    uint32_t len;
    uint8_t revision;
    uint8_t checksum;
    char oemid[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct ACPI_RSDT {
    struct ACPI_SDT_HEADER header;
    uint32_t entry[1];
};

struct ACPI_XSDT {
    struct ACPI_SDT_HEADER header;
    uint64_t entry[1];
};

typedef struct {
    uint8_t address_space;
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} GENERIC_ADDRESS_STRUCT;

struct ACPI_FADT {
    struct ACPI_SDT_HEADER header;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved;
    uint8_t preferred_power_management_profile;
    uint16_t sci_interrupt;
    uint32_t smi_command_port;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_control;
    uint32_t pm1a_event_block;
    uint32_t pm1b_event_block;
    uint32_t pm1a_control_block;
    uint32_t pm2_control_block;
    uint32_t pm_timer_block;
    uint32_t gpe0_block;
    uint32_t gpe1_block;
    uint8_t pm1_event_len;
    uint8_t pm1_control_len;
    uint8_t pm2_control_len;
    uint8_t pm_timer_len;
    uint8_t gpe0_len;
    uint8_t gpe1_len;
    uint8_t gpe1_base;
    uint8_t cstate_control;
    uint16_t worst_c2_latency;
    uint16_t worst_c3_latency;
    uint16_t flush_size;
    uint16_t flush_stride;
    uint8_t duty_offset;
    uint8_t duty_width;
    uint8_t day_alarm;
    uint8_t month_alarm;
    uint8_t century;
    uint16_t boot_architecture_flags;
    uint8_t reserved2;
    uint32_t flags;
    GENERIC_ADDRESS_STRUCT reset_reg;
    uint8_t reset_value;
    uint8_t reserved3[3];
    uint64_t x_firmware_control;
    uint64_t x_dsdt;
    GENERIC_ADDRESS_STRUCT x_pm1a_event_block;
    GENERIC_ADDRESS_STRUCT x_pm1b_event_block;
    GENERIC_ADDRESS_STRUCT x_pm1a_control_block;
    GENERIC_ADDRESS_STRUCT x_pm1b_control_block;
    GENERIC_ADDRESS_STRUCT x_pm2_control_block;
    GENERIC_ADDRESS_STRUCT x_pm_timer_block;
    GENERIC_ADDRESS_STRUCT x_gpe0_block;
    GENERIC_ADDRESS_STRUCT x_gpe1_block;
};

void acpi_init(void);
void acpi_shutdown(void);

extern struct ACPI_RSDP *RSDP;
extern struct ACPI_XSDT *XSDT;
extern struct ACPI_FADT *FADT;

#endif
