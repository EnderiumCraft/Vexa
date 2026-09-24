#include <stdbool.h>
#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/fs.h>
#include <vexa/io.h>
#include <vexa/kprintf.h>

/* The CMOS real-time clock: read once at boot. After that the time is the boot
 * time plus the timer's uptime, which is plenty for file timestamps. */

#define CMOS_ADDRESS 0x70
#define CMOS_DATA 0x71

static long long boot_time;

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, reg | 0x80); /* Bit 7 keeps NMIs disabled while we're here. */
    return inb(CMOS_DATA);
}

static bool update_in_progress(void) {
    return cmos_read(0x0a) & 0x80;
}

static long long days_from_civil(long long y, unsigned m, unsigned d) {
    /* Days since 1970-01-01 (Howard Hinnant's algorithm). */
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

void rtc_init(void) {
    uint8_t second, minute, hour, day, month, year, status_b;
    /* Read twice until both agree, so we never catch it mid-update. */
    for (int tries = 0; tries < 5; tries++) {
        while (update_in_progress()) {
        }
        second = cmos_read(0x00), minute = cmos_read(0x02), hour = cmos_read(0x04);
        day = cmos_read(0x07), month = cmos_read(0x08), year = cmos_read(0x09);
        while (update_in_progress()) {
        }
        if (second == cmos_read(0x00) && minute == cmos_read(0x02) && hour == cmos_read(0x04) &&
            day == cmos_read(0x07)) {
            break;
        }
    }
    status_b = cmos_read(0x0b);
    bool pm = hour & 0x80;
    hour &= 0x7f;
    if (!(status_b & 0x04)) { /* BCD, the usual encoding. */
#define BCD(x) (((x) & 0x0f) + ((x) >> 4) * 10)
        second = BCD(second), minute = BCD(minute), hour = BCD(hour);
        day = BCD(day), month = BCD(month), year = BCD(year);
#undef BCD
    }
    if (!(status_b & 0x02) && pm) { /* 12-hour clock. */
        hour = (hour % 12) + 12;
    }
    long long days = days_from_civil(2000 + year, month, day);
    boot_time = days * 86400 + hour * 3600 + minute * 60 + second - (long long)(timer_ms() / 1000);
    kprintf("[rtc] %u-%s%u-%s%u %s%u:%s%u UTC\n", 2000 + year, month < 10 ? "0" : "", month,
            day < 10 ? "0" : "", day, hour < 10 ? "0" : "", hour, minute < 10 ? "0" : "", minute);
}

long long time_now(void) {
    return boot_time ? boot_time + (long long)(timer_ms() / 1000) : 0;
}
