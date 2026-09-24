#ifndef VEXA_RANDOM_H
#define VEXA_RANDOM_H

#include <stddef.h>

/* Unpredictable bytes: from the CPU's RDRAND where available, mixed with the
 * cycle counter. Good enough for stack canaries and hash seeds, not for keys. */
void random_bytes(void *buffer, size_t size);

#endif
