#ifndef APIC_H
#define APIC_H

#include <stdint.h>

int apic_init(void);
int apic_active(void);

void lapic_eoi(void);

uint32_t lapic_get_id(void);
void lapic_send_ipi_init(uint32_t apic_id);
void lapic_send_ipi_sipi(uint32_t apic_id, uint32_t vector);

#endif
