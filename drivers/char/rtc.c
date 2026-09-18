#include "drivers/char/rtc.h"

#include "kernel/asm_func.h"

#define RTC_REG_INDEX 0x70
#define RTC_REG_DATA 0x71
#define RTC_REG_SEC 0x00
#define RTC_REG_MIN 0x02
#define RTC_REG_HOUR 0x04
#define RTC_REG_STATUS_A 0x0A
#define RTC_REG_STATUS_B 0x0B

static uint8_t cmos_read(uint8_t reg) {
    outb(RTC_REG_INDEX, reg);
    return inb(RTC_REG_DATA);
}

static uint8_t from_bcd(uint8_t v) {
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

int rtc_read_time(uint8_t *hour, uint8_t *min, uint8_t *sec) {
    uint8_t status_b = cmos_read(RTC_REG_STATUS_B);
    uint8_t h;
    uint8_t m;
    uint8_t s;
    for (int i = 0; i < 64; i++) {
        if (!(cmos_read(RTC_REG_STATUS_A) & 0x80))
            break;
    }
    s = cmos_read(RTC_REG_SEC);
    m = cmos_read(RTC_REG_MIN);
    h = cmos_read(RTC_REG_HOUR);
    if (!(status_b & 0x04)) {
        s = from_bcd(s);
        m = from_bcd(m);
        h = from_bcd(h);
    }
    if (!(status_b & 0x02)) {
        int pm = (h & 0x80) != 0;
        h = (uint8_t)(h & 0x7F);
        if (pm && h != 12)
            h = (uint8_t)(h + 12);
        if (!pm && h == 12)
            h = 0;
    }
    if (h >= 24)
        h = 0;
    if (m >= 60)
        m = 0;
    if (s >= 60)
        s = 0;
    *hour = h;
    *min = m;
    *sec = s;
    return 0;
}
