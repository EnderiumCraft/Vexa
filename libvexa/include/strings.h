#ifndef LIBVEXA_STRINGS_H
#define LIBVEXA_STRINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
int ffs(int value);
void bzero(void *p, size_t n);
int bcmp(const void *a, const void *b, size_t n);

#ifdef __cplusplus
}
#endif

#endif
