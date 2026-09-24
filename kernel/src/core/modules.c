#include <vexa/kprintf.h>
#include <vexa/modules.h>
#include <vexa/string.h>

#define MAX_MODULES 16

static struct boot_module modules[MAX_MODULES];
static size_t module_count;

void modules_init(struct limine_module_response *response) {
    if (!response) {
        return;
    }
    for (uint64_t i = 0; i < response->module_count && module_count < MAX_MODULES; i++) {
        struct limine_file *file = response->modules[i];
        /* The name is the last part of the path: boot():/boot/initramfs.tar */
        const char *name = file->path;
        for (const char *p = file->path; *p; p++) {
            if (*p == '/') {
                name = p + 1;
            }
        }
        struct boot_module *module = &modules[module_count++];
        size_t n = 0;
        for (; name[n] && n < sizeof(module->name) - 1; n++) {
            module->name[n] = name[n];
        }
        module->name[n] = '\0';
        module->data = file->address;
        module->size = file->size;
    }
}

const struct boot_module *module_find(const char *name) {
    for (size_t i = 0; i < module_count; i++) {
        if (strcmp(modules[i].name, name) == 0) {
            return &modules[i];
        }
    }
    return NULL;
}
