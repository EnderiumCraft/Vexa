/* <time.h>: clocks, sleeping, calendar times. Local time is the zone set in
 * Settings (vexa/time.h's zones, with summer time). */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <vexa/settings.h>
#include <vexa/syscall.h>
#include <vexa/time.h>

/* The real-time clock counts seconds; within one, the uptime clock fills in
 * the milliseconds (taken when the second last changed). */
static long base_seconds, base_uptime;

static void realtime_ms(long long *ms) {
    long now = vx_time(), up = vx_uptime();
    if (now != base_seconds) {
        base_seconds = now;
        base_uptime = up;
    }
    long within = up - base_uptime;
    *ms = (long long)now * 1000 + (within < 1000 ? within : 999);
}

int clock_gettime(clockid_t clock, struct timespec *ts) {
    if (clock == CLOCK_REALTIME) {
        long long ms;
        realtime_ms(&ms);
        ts->tv_sec = (time_t)(ms / 1000);
        ts->tv_nsec = (long)(ms % 1000) * 1000000;
        return 0;
    }
    long up = vx_uptime();
    ts->tv_sec = up / 1000;
    ts->tv_nsec = (up % 1000) * 1000000;
    return 0;
}

int clock_getres(clockid_t clock, struct timespec *ts) {
    (void)clock;
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = 1000000;
    }
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    long long ms;
    realtime_ms(&ms);
    tv->tv_sec = (time_t)(ms / 1000);
    tv->tv_usec = (suseconds_t)(ms % 1000) * 1000;
    return 0;
}

time_t time(time_t *out) {
    time_t now = vx_time();
    if (out) {
        *out = now;
    }
    return now;
}

clock_t clock(void) {
    return (clock_t)vx_uptime() * (CLOCKS_PER_SEC / 1000);
}

double difftime(time_t end, time_t start) {
    return (double)(end - start);
}

int nanosleep(const struct timespec *request, struct timespec *remaining) {
    if (request->tv_nsec < 0 || request->tv_nsec >= 1000000000) {
        errno = EINVAL;
        return -1;
    }
    long long ms = (long long)request->tv_sec * 1000 + (request->tv_nsec + 999999) / 1000000;
    vx_sleep((uint64_t)ms);
    if (remaining) {
        remaining->tv_sec = 0;
        remaining->tv_nsec = 0;
    }
    return 0;
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                    struct timespec *remaining) {
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        clock_gettime(clock, &now);
        long long ms = ((long long)request->tv_sec - now.tv_sec) * 1000 +
                       (request->tv_nsec - now.tv_nsec) / 1000000;
        if (ms > 0) {
            vx_sleep((uint64_t)ms);
        }
        return 0;
    }
    return nanosleep(request, remaining) ? errno : 0;
}

/* ---- Calendar ---- */

static int days_before_month(int year, int month) {
    static const int days[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return days[month] + (leap && month > 1);
}

static struct tm *broken_down(long seconds, long offset, const char *zone, struct tm *out) {
    struct vx_date d;
    vx_date_of(seconds + offset, &d);
    out->tm_sec = d.second;
    out->tm_min = d.minute;
    out->tm_hour = d.hour;
    out->tm_mday = d.day;
    out->tm_mon = d.month - 1;
    out->tm_year = d.year - 1900;
    out->tm_wday = d.weekday;
    out->tm_yday = days_before_month(d.year, d.month - 1) + d.day - 1;
    out->tm_isdst = 0;
    out->tm_gmtoff = offset;
    out->tm_zone = zone;
    return out;
}

struct tm *gmtime_r(const time_t *t, struct tm *out) {
    return broken_down(*t, 0, "UTC", out);
}

struct tm *gmtime(const time_t *t) {
    static struct tm tm;
    return gmtime_r(t, &tm);
}

char *tzname[2] = {"UTC", "UTC"};
long timezone;
int daylight;

/* The offset from UTC (seconds) at a moment, from Settings. */
static long local_offset(long utc) {
    struct vx_settings s;
    if (vx_settings_load(&s, "desktop.conf")) {
        return 0;
    }
    const struct vx_zone *zone = vx_find_zone(vx_settings_get(&s, "time_zone", NULL));
    return 60L * (zone ? vx_zone_offset(zone, utc) : vx_settings_int(&s, "utc_offset", 0));
}

void tzset(void) {
    timezone = -local_offset(vx_time());
}

struct tm *localtime_r(const time_t *t, struct tm *out) {
    long offset = local_offset(*t), half_a_year_ago = local_offset(*t - 182 * 86400L);
    broken_down(*t, offset, "local", out);
    out->tm_isdst = offset > half_a_year_ago; /* (Summer time is the bigger offset.) */
    return out;
}

struct tm *localtime(const time_t *t) {
    static struct tm tm;
    return localtime_r(t, &tm);
}

time_t timegm(struct tm *tm) {
    /* Normalize months, then count days from 1970. */
    long year = tm->tm_year + 1900L + tm->tm_mon / 12, month = tm->tm_mon % 12;
    if (month < 0) {
        month += 12;
        year--;
    }
    struct vx_date d = {(int)year, (int)month + 1, 1, 0, 0, 0, 0};
    long seconds = vx_seconds_of(&d) + (tm->tm_mday - 1) * 86400L + tm->tm_hour * 3600L +
                   tm->tm_min * 60L + tm->tm_sec;
    broken_down(seconds, 0, "UTC", tm);
    return seconds;
}

time_t mktime(struct tm *tm) {
    struct tm copy = *tm;
    long guess = timegm(&copy);
    long seconds = guess - local_offset(guess);
    localtime_r(&seconds, tm);
    return seconds;
}

static const char *const short_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const short_months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

size_t strftime(char *out, size_t size, const char *f, const struct tm *tm) {
    size_t n = 0;
    char piece[64];
    for (; *f; f++) {
        if (*f != '%') {
            if (n + 1 >= size) {
                return 0;
            }
            out[n++] = *f;
            continue;
        }
        f++;
        while (*f == 'E' || *f == 'O' || *f == '-' || *f == '_' || *f == '0') {
            f++;
        }
        int hour12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
        switch (*f) {
        case 'a': snprintf(piece, sizeof(piece), "%s", short_days[tm->tm_wday % 7]); break;
        case 'A': snprintf(piece, sizeof(piece), "%s", vx_weekday_names[tm->tm_wday % 7]); break;
        case 'b':
        case 'h': snprintf(piece, sizeof(piece), "%s", short_months[tm->tm_mon % 12]); break;
        case 'B': snprintf(piece, sizeof(piece), "%s", vx_month_names[tm->tm_mon % 12]); break;
        case 'c':
            snprintf(piece, sizeof(piece), "%s %s %2d %02d:%02d:%02d %d", short_days[tm->tm_wday % 7],
                     short_months[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min,
                     tm->tm_sec, tm->tm_year + 1900);
            break;
        case 'C': snprintf(piece, sizeof(piece), "%02d", (tm->tm_year + 1900) / 100); break;
        case 'd': snprintf(piece, sizeof(piece), "%02d", tm->tm_mday); break;
        case 'e': snprintf(piece, sizeof(piece), "%2d", tm->tm_mday); break;
        case 'D':
            snprintf(piece, sizeof(piece), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_year % 100);
            break;
        case 'F':
            snprintf(piece, sizeof(piece), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1,
                     tm->tm_mday);
            break;
        case 'H': snprintf(piece, sizeof(piece), "%02d", tm->tm_hour); break;
        case 'I': snprintf(piece, sizeof(piece), "%02d", hour12); break;
        case 'j': snprintf(piece, sizeof(piece), "%03d", tm->tm_yday + 1); break;
        case 'm': snprintf(piece, sizeof(piece), "%02d", tm->tm_mon + 1); break;
        case 'M': snprintf(piece, sizeof(piece), "%02d", tm->tm_min); break;
        case 'n': snprintf(piece, sizeof(piece), "\n"); break;
        case 't': snprintf(piece, sizeof(piece), "\t"); break;
        case 'p': snprintf(piece, sizeof(piece), "%s", tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'r':
            snprintf(piece, sizeof(piece), "%02d:%02d:%02d %s", hour12, tm->tm_min, tm->tm_sec,
                     tm->tm_hour < 12 ? "AM" : "PM");
            break;
        case 'R': snprintf(piece, sizeof(piece), "%02d:%02d", tm->tm_hour, tm->tm_min); break;
        case 'S': snprintf(piece, sizeof(piece), "%02d", tm->tm_sec); break;
        case 's': {
            struct tm copy = *tm;
            snprintf(piece, sizeof(piece), "%ld", (long)timegm(&copy) - tm->tm_gmtoff);
            break;
        }
        case 'T':
        case 'X':
            snprintf(piece, sizeof(piece), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
            break;
        case 'x':
            snprintf(piece, sizeof(piece), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday,
                     tm->tm_year % 100);
            break;
        case 'u': snprintf(piece, sizeof(piece), "%d", tm->tm_wday ? tm->tm_wday : 7); break;
        case 'w': snprintf(piece, sizeof(piece), "%d", tm->tm_wday); break;
        case 'U':
            snprintf(piece, sizeof(piece), "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7);
            break;
        case 'W':
            snprintf(piece, sizeof(piece), "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7);
            break;
        case 'y': snprintf(piece, sizeof(piece), "%02d", tm->tm_year % 100); break;
        case 'Y': snprintf(piece, sizeof(piece), "%d", tm->tm_year + 1900); break;
        case 'z': {
            long o = tm->tm_gmtoff / 60;
            snprintf(piece, sizeof(piece), "%c%02ld%02ld", o < 0 ? '-' : '+', (o < 0 ? -o : o) / 60,
                     (o < 0 ? -o : o) % 60);
            break;
        }
        case 'Z': snprintf(piece, sizeof(piece), "%s", tm->tm_zone ? tm->tm_zone : "UTC"); break;
        case '%': snprintf(piece, sizeof(piece), "%%"); break;
        case '\0': f--; piece[0] = '\0'; break;
        default: snprintf(piece, sizeof(piece), "%%%c", *f); break;
        }
        size_t length = strlen(piece);
        if (n + length >= size) {
            return 0;
        }
        memcpy(out + n, piece, length);
        n += length;
    }
    out[n] = '\0';
    return n;
}

char *asctime(const struct tm *tm) {
    static char text[32];
    snprintf(text, sizeof(text), "%s %s %2d %02d:%02d:%02d %d\n", short_days[tm->tm_wday % 7],
             short_months[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return text;
}

char *ctime(const time_t *t) {
    return asctime(localtime(t));
}
