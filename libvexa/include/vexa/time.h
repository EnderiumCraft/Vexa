#ifndef VEXA_TIME_H
#define VEXA_TIME_H

#include <stdbool.h>

/*
 * Dates and time zones. Vexa's clock (vx_time) is UTC; a zone gives the
 * local time: a city, its offset from UTC, and when it has summer time
 * (the European, North American, southern Australian and New Zealand
 * rules; places without summer time have none).
 */

struct vx_date {
    int year, month, day;       /* month 1-12, day 1-31 */
    int hour, minute, second;
    int weekday;                /* 0 Sunday ... 6 Saturday */
};

enum vx_dst { VX_DST_NONE, VX_DST_EU, VX_DST_US, VX_DST_AU, VX_DST_NZ };

struct vx_zone {
    const char *city, *region; /* "Berlin", "Europe" */
    int offset;                /* Minutes east of UTC, in winter (standard time). */
    enum vx_dst dst;
};

extern const struct vx_zone vx_zones[];
extern const int vx_zone_count;

/* A zone by city name, or NULL. */
const struct vx_zone *vx_find_zone(const char *city);
/* The zone's offset from UTC at a moment (with summer time), in minutes. */
int vx_zone_offset(const struct vx_zone *zone, long utc_seconds);
/* Seconds since 1970 (UTC) to a date, and back. */
void vx_date_of(long seconds, struct vx_date *date);
long vx_seconds_of(const struct vx_date *date);
/* The local time now, from the settings in /etc/desktop.conf ("time_zone",
 * a city; or "utc_offset" in minutes). */
void vx_local_now(struct vx_date *date);

extern const char *const vx_month_names[12];   /* "January"... */
extern const char *const vx_weekday_names[7];  /* "Sunday"... */

#endif
