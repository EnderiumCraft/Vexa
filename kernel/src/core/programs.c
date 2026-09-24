#include <vexa/kprintf.h>
#include <vexa/programs.h>
#include <vexa/string.h>

#define MAX_PROGRAMS 32

static struct boot_program programs[MAX_PROGRAMS];
static size_t program_count;

void programs_init(struct limine_module_response *modules) {
    if (!modules) {
        return;
    }
    for (uint64_t i = 0; i < modules->module_count && program_count < MAX_PROGRAMS; i++) {
        struct limine_file *file = modules->modules[i];
        /* The name is the last part of the path: boot():/boot/programs/hello-world */
        const char *name = file->path;
        for (const char *p = file->path; *p; p++) {
            if (*p == '/') {
                name = p + 1;
            }
        }
        struct boot_program *program = &programs[program_count++];
        size_t n = 0;
        for (; name[n] && n < sizeof(program->name) - 1; n++) {
            program->name[n] = name[n];
        }
        program->name[n] = '\0';
        program->data = file->address;
        program->size = file->size;
    }
    kprintf("[boot] %lu program(s) loaded by the bootloader\n", program_count);
}

const struct boot_program *programs_find(const char *name) {
    for (size_t i = 0; i < program_count; i++) {
        if (strcmp(programs[i].name, name) == 0) {
            return &programs[i];
        }
    }
    return NULL;
}

const struct boot_program *programs_get(size_t index) {
    return index < program_count ? &programs[index] : NULL;
}
