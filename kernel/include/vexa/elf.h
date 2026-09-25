#ifndef VEXA_ELF_H
#define VEXA_ELF_H

#include <stddef.h>
#include <stdint.h>

struct address_space;
struct file;
struct personality;

#define ELF_INTERP_MAX 256

struct elf_image {
    uint64_t entry;
    uint64_t base;         /* Where an ET_DYN file was put (0 for ET_EXEC). */
    uint64_t phdr_address; /* Where the program headers are in memory (for AT_PHDR). */
    uint16_t phent, phnum;
    uint64_t heap_start;   /* First page after the highest segment. */
    const struct personality *personality;
    char interp[ELF_INTERP_MAX]; /* PT_INTERP: the dynamic loader to run, or "". */
};

/* Where position-independent files go: programs (like Linux), and dynamic
 * loaders, far above them and below the stack. */
#define ELF_PROGRAM_BASE 0x0000555555554000ULL
#define ELF_INTERP_BASE 0x00007fc000000000ULL

/* Loads an x86_64 ELF executable from an open file into `as`, adding a memory
 * area per segment; an ET_DYN file goes at `base`. Picks the personality:
 * native if the program has the Vexa note, Linux otherwise (if the kernel was
 * built with it). A dynamically linked program's loader is named in
 * image->interp for the caller to load. Returns 0, or a negative VX_E* error
 * with *reason explaining it. */
int elf_load(struct address_space *as, struct file *file, uint64_t base,
             struct elf_image *image, const char **reason);

#endif
