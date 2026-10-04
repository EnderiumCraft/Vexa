#ifndef LIBVEXA_TIME_H
#define LIBVEXA_TIME_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCKS_PER_SEC 1000000L
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define TIMER_ABSTIME 1

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec, tm_min, tm_hour; /* 0-60, 0-59, 0-23 */
    int tm_mday, tm_mon, tm_year; /* 1-31, 0-11, years since 1900 */
    int tm_wday, tm_yday;         /* 0-6 from Sunday, 0-365 */
    int tm_isdst;
    long tm_gmtoff;               /* Seconds east of UTC. */
    const char *tm_zone;
};

time_t time(time_t *out);
clock_t clock(void);
double difftime(time_t end, time_t start);
int clock_gettime(clockid_t clock, struct timespec *ts);
int clock_getres(clockid_t clock, struct timespec *ts);
int nanosleep(const struct timespec *request, struct timespec *remaining);
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request,
                    struct timespec *remaining);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
/* Local time is the time zone set in Settings (/etc/desktop.conf). */
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);
size_t strftime(char *out, size_t size, const char *format, const struct tm *tm);
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);
void tzset(void);
extern char *tzname[2];
extern long timezone;
extern int daylight;

#ifdef __cplusplus
}
#endif

#endif
