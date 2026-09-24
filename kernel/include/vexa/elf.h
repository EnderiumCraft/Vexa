#ifndef VEXA_ELF_H
#define VEXA_ELF_H

#include <stddef.h>
#include <stdint.h>

struct address_space;
struct file;
struct personality;

struct elf_image {
    uint64_t entry;
    uint64_t phdr_address; /* Where the program headers are in memory (for AT_PHDR). */
    uint16_t phent, phnum;
    uint64_t heap_start;   /* First page after the highest segment. */
    const struct personality *personality;
};

/* Loads a static x86_64 ELF executable from an open file into `as`, adding a
 * memory area per segment. Picks the personality: native if the program has
 * the Vexa note, Linux otherwise (if the kernel was built with it). Returns 0,
 * or a negative VX_E* error with *reason explaining it. */
int elf_load(struct address_space *as, struct file *file, struct elf_image *image,
             const char **reason);

#endif
