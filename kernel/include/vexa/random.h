#ifndef VEXA_RANDOM_H
#define VEXA_RANDOM_H

#include <stddef.h>

/* Unpredictable bytes from a cryptographic generator (ChaCha20, keyed from a
 * BLAKE2s pool of the CPU's RDSEED/RDRAND, the cycle counter and interrupt
 * timings): good for keys. Callable with interrupts off. */
void random_bytes(void *buffer, size_t size);

/* Mixes data (unpredictable or not; it can't hurt) into the pool. */
void random_add(const void *data, size_t size);

/* Called on every interrupt: its timing goes into the pool. */
void random_interrupt(unsigned vector);

#endif
