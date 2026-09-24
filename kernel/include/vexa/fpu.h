#ifndef VEXA_FPU_H
#define VEXA_FPU_H

#include <stddef.h>

/* Floating point and vector register state (x87, SSE, AVX...). The kernel
 * itself never uses these registers, so only user threads carry a saved copy,
 * and it is swapped on every context switch. */

/* Enables the FPU, SSE and (where available) XSAVE and AVX on this CPU. */
void fpu_init_cpu(void);
/* A fresh register state for a new thread, or NULL if out of memory. */
void *fpu_alloc_state(void);
void fpu_free_state(void *state);
void fpu_save(void *state);
void fpu_restore(void *state);
/* Describes what is enabled, e.g. "XSAVE, AVX (832 bytes)". */
const char *fpu_describe(void);

#endif
