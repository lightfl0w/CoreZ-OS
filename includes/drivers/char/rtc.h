#ifndef DRIVERS_RTC_H
#define DRIVERS_RTC_H
#include <stdint.h>

int rtc_read_time(uint8_t *hour, uint8_t *min, uint8_t *sec);

#endif
