#ifndef APIC_H
#define APIC_H

#include <stdint.h>

int apic_init(void);
int apic_active(void);
int lapic_timer_on(void);

void lapic_eoi(void);

uint32_t lapic_get_id(void);
void lapic_send_ipi_init(uint32_t apic_id);
void lapic_send_ipi_sipi(uint32_t apic_id, uint32_t vector);

#define IPI_VECTOR_RESCHED 0x82
#define LAPIC_CALIB_VECTOR 0x70

extern volatile uint32_t lapic_calib_count;

void lapic_ap_enable(void);
void lapic_timer_program_periodic(void);
void lapic_send_ipi_all_but_self(uint32_t vector);

#endif
