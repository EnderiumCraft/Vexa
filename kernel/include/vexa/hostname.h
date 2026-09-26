#ifndef VEXA_HOSTNAME_H
#define VEXA_HOSTNAME_H

#include <stddef.h>

/* The computer's name ("vexa" until it's set): uname's node name. */
#define HOSTNAME_MAX 64
void hostname_get(char out[HOSTNAME_MAX + 1]);
/* Letters, digits, '-' and '.', 1 to HOSTNAME_MAX of them: 0 or -VX_EINVAL. */
int hostname_set(const char *name, size_t length);

#endif
