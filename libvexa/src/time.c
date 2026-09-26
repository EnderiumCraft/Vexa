#include <string.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/time.h>

/* Dates and time zones (see <vexa/time.h>). */

const char *const vx_month_names[12] = {
    "January", "February", "March", "April", "May", "June", "July", "August", "September",
    "October", "November", "December",
};
const char *const vx_weekday_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
};

/* By offset, west to east. */
const struct vx_zone vx_zones[] = {
    {"Honolulu", "Pacific", -600, VX_DST_NONE},
    {"Anchorage", "America", -540, VX_DST_US},
    {"Los Angeles", "America", -480, VX_DST_US},
    {"Vancouver", "America", -480, VX_DST_US},
    {"Denver", "America", -420, VX_DST_US},
    {"Phoenix", "America", -420, VX_DST_NONE},
    {"Chicago", "America", -360, VX_DST_US},
    {"Mexico City", "America", -360, VX_DST_NONE},
    {"New York", "America", -300, VX_DST_US},
    {"Toronto", "America", -300, VX_DST_US},
    {"Bogota", "America", -300, VX_DST_NONE},
    {"Halifax", "America", -240, VX_DST_US},
    {"Sao Paulo", "America", -180, VX_DST_NONE},
    {"Buenos Aires", "America", -180, VX_DST_NONE},
    {"Reykjavik", "Atlantic", 0, VX_DST_NONE},
    {"London", "Europe", 0, VX_DST_EU},
    {"Dublin", "Europe", 0, VX_DST_EU},
    {"Lisbon", "Europe", 0, VX_DST_EU},
    {"Lagos", "Africa", 60, VX_DST_NONE},
    {"Paris", "Europe", 60, VX_DST_EU},
    {"Berlin", "Europe", 60, VX_DST_EU},
    {"Madrid", "Europe", 60, VX_DST_EU},
    {"Rome", "Europe", 60, VX_DST_EU},
    {"Amsterdam", "Europe", 60, VX_DST_EU},
    {"Stockholm", "Europe", 60, VX_DST_EU},
    {"Warsaw", "Europe", 60, VX_DST_EU},
    {"Cairo", "Africa", 120, VX_DST_NONE},
    {"Johannesburg", "Africa", 120, VX_DST_NONE},
    {"Athens", "Europe", 120, VX_DST_EU},
    {"Helsinki", "Europe", 120, VX_DST_EU},
    {"Kyiv", "Europe", 120, VX_DST_EU},
    {"Istanbul", "Europe", 180, VX_DST_NONE},
    {"Moscow", "Europe", 180, VX_DST_NONE},
    {"Nairobi", "Africa", 180, VX_DST_NONE},
    {"Riyadh", "Asia", 180, VX_DST_NONE},
    {"Dubai", "Asia", 240, VX_DST_NONE},
    {"Karachi", "Asia", 300, VX_DST_NONE},
    {"Tashkent", "Asia", 300, VX_DST_NONE},
    {"Delhi", "Asia", 330, VX_DST_NONE},
    {"Kathmandu", "Asia", 345, VX_DST_NONE},
    {"Dhaka", "Asia", 360, VX_DST_NONE},
    {"Bangkok", "Asia", 420, VX_DST_NONE},
    {"Jakarta", "Asia", 420, VX_DST_NONE},
    {"Beijing", "Asia", 480, VX_DST_NONE},
    {"Singapore", "Asia", 480, VX_DST_NONE},
    {"Perth", "Australia", 480, VX_DST_NONE},
    {"Tokyo", "Asia", 540, VX_DST_NONE},
    {"Seoul", "Asia", 540, VX_DST_NONE},
    {"Adelaide", "Australia", 570, VX_DST_AU},
    {"Brisbane", "Australia", 600, VX_DST_NONE},
    {"Sydney", "Australia", 600, VX_DST_AU},
    {"Melbourne", "Australia", 600, VX_DST_AU},
    {"Noumea", "Pacific", 660, VX_DST_NONE},
    {"Auckland", "Pacific", 720, VX_DST_NZ},
};
const int vx_zone_count = sizeof(vx_zones) / sizeof(vx_zones[0]);

const struct vx_zone *vx_find_zone(const char *city) {
    for (int i = 0; city && i < vx_zone_count; i++) {
        if (!strcmp(vx_zones[i].city, city)) {
            return &vx_zones[i];
        }
    }
    return NULL;
}

/* Days since 1970 of a civil date, and back (Howard Hinnant's algorithms). */
static long days_from_civil(long y, int m, int d) {
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void vx_date_of(long seconds, struct vx_date *date) {
    long days = seconds / 86400, rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        days--;
    }
    long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    date->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    date->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    date->year = (int)(yoe + era * 400 + (date->month <= 2));
    date->hour = (int)(rest / 3600);
    date->minute = (int)(rest / 60 % 60);
    date->second = (int)(rest % 60);
    date->weekday = (int)((days % 7 + 11) % 7); /* 1970-01-01 was a Thursday. */
}

long vx_seconds_of(const struct vx_date *d) {
    return days_from_civil(d->year, d->month, d->day) * 86400 + d->hour * 3600 + d->minute * 60 +
           d->second;
}

/* The day of the month of a month's `n`th (1...; 0: last) given weekday. */
static int nth_weekday(int year, int month, int weekday, int n) {
    int first = (int)((days_from_civil(year, month, 1) % 7 + 11) % 7);
    int day = 1 + (weekday - first + 7) % 7;
    if (n > 0) {
        return day + 7 * (n - 1);
    }
    static const int lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int length = lengths[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 || year % 400 == 0));
    while (day + 7 <= length) {
        day += 7;
    }
    return day;
}

/* UTC seconds of a local time in a zone's standard time. */
static long at(int year, int month, int day, int hour, int offset) {
    struct vx_date d = {year, month, day, hour, 0, 0, 0};
    return vx_seconds_of(&d) - offset * 60L;
}

int vx_zone_offset(const struct vx_zone *zone, long utc) {
    if (!zone) {
        return 0;
    }
    struct vx_date now;
    vx_date_of(utc + zone->offset * 60L, &now);
    int y = now.year, o = zone->offset;
    bool summer = false;
    switch (zone->dst) {
    case VX_DST_NONE:
        break;
    case VX_DST_EU: /* Last Sunday of March to the last of October, at 01:00 UTC. */
        summer = utc >= at(y, 3, nth_weekday(y, 3, 0, 0), 1, 0) &&
                 utc < at(y, 10, nth_weekday(y, 10, 0, 0), 1, 0);
        break;
    case VX_DST_US: /* Second Sunday of March to the first of November, 02:00 local. */
        summer = utc >= at(y, 3, nth_weekday(y, 3, 0, 2), 2, o) &&
                 utc < at(y, 11, nth_weekday(y, 11, 0, 1), 2, o + 60);
        break;
    case VX_DST_AU: /* First Sunday of October to the first of April, 02:00 local. */
        summer = !(utc >= at(y, 4, nth_weekday(y, 4, 0, 1), 3, o + 60) &&
                   utc < at(y, 10, nth_weekday(y, 10, 0, 1), 2, o));
        break;
    case VX_DST_NZ: /* Last Sunday of September to the first of April, 02:00 local. */
        summer = !(utc >= at(y, 4, nth_weekday(y, 4, 0, 1), 3, o + 60) &&
                   utc < at(y, 9, nth_weekday(y, 9, 0, 0), 2, o));
        break;
    }
    return o + (summer ? 60 : 0);
}

void vx_local_now(struct vx_date *date) {
    struct vx_settings s;
    vx_settings_load(&s, "desktop.conf");
    long now = vx_time();
    const struct vx_zone *zone = vx_find_zone(vx_settings_get(&s, "time_zone", NULL));
    int offset = zone ? vx_zone_offset(zone, now) : vx_settings_int(&s, "utc_offset", 0);
    vx_date_of(now + offset * 60L, date);
}
