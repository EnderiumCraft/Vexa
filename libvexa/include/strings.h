#ifndef LIBVEXA_STRINGS_H
#define LIBVEXA_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
int ffs(int value);
void bzero(void *p, size_t n);

#endif
