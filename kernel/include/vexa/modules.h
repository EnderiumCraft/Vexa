#ifndef VEXA_MODULES_H
#define VEXA_MODULES_H

#include <stddef.h>
#include <limine.h>

/* Files the bootloader loaded next to the kernel (module_path lines in
 * limine.conf). Vexa uses one: initramfs.tar, the starting root file system. */
struct boot_module {
    char name[32];
    const void *data; /* In module memory, which is never reclaimed. */
    size_t size;
};

/* Copies the module list out of bootloader memory. */
void modules_init(struct limine_module_response *modules);
const struct boot_module *module_find(const char *name);

#endif
