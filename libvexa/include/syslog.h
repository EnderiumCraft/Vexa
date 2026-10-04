#ifndef LIBVEXA_SYSLOG_H
#define LIBVEXA_SYSLOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7

#define LOG_PID 0x01
#define LOG_CONS 0x02
#define LOG_NDELAY 0x08
#define LOG_NOWAIT 0x10
#define LOG_PERROR 0x20

#define LOG_KERN (0 << 3)
#define LOG_USER (1 << 3)
#define LOG_DAEMON (3 << 3)
#define LOG_AUTH (4 << 3)
#define LOG_LOCAL0 (16 << 3)
#define LOG_LOCAL7 (23 << 3)

#define LOG_MASK(priority) (1 << (priority))
#define LOG_UPTO(priority) ((1 << ((priority) + 1)) - 1)

/* There's no system log: messages go to standard error, as
 * "<ident>[<pid>]: <message>". */
void openlog(const char *ident, int options, int facility);
void syslog(int priority, const char *format, ...) __attribute__((format(printf, 2, 3)));
void vsyslog(int priority, const char *format, va_list args);
void closelog(void);
int setlogmask(int mask);

#ifdef __cplusplus
}
#endif

#endif
