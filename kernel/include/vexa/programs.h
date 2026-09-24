#ifndef VEXA_PROGRAMS_H
#define VEXA_PROGRAMS_H

#include <stddef.h>
#include <limine.h>

/* Programs the bootloader loaded as modules (listed in limine.conf). Until
 * Vexa has a file system (Phase 4), this is where `run` finds programs. */
struct boot_program {
    char name[32];
    const void *data; /* In module memory, which is never reclaimed. */
    size_t size;
};

/* Copies the module list out of bootloader memory. */
void programs_init(struct limine_module_response *modules);
const struct boot_program *programs_find(const char *name);
const struct boot_program *programs_get(size_t index); /* NULL past the end. */

#endif
