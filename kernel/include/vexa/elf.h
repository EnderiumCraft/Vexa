#ifndef VEXA_ELF_H
#define VEXA_ELF_H

#include <stddef.h>
#include <stdint.h>

struct address_space;
struct personality;

/* Loads a static x86_64 ELF executable into `as`. On success returns 0 and
 * sets the entry point and the personality its notes ask for. On failure
 * returns -1 and sets *reason. */
int elf_load(struct address_space *as, const void *data, size_t size, uint64_t *entry,
             const struct personality **personality, const char **reason);

#endif
