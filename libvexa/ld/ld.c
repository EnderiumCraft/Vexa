/*
 * vexa-ld.so: Vexa's dynamic loader, for native programs linked against
 * libvexa.so.
 *
 * The kernel maps the program and this loader and starts the loader, with
 * the auxiliary vector saying where both are. The loader relocates itself,
 * loads each library the program needs from /lib (DT_NEEDED), resolves
 * symbols (program first, then the libraries in order), applies the
 * relocations, sets the final page permissions, runs the libraries'
 * initializers, and jumps to the program.
 *
 * It uses nothing but system calls: it runs before libvexa is there. It
 * supports what x86_64 position-independent code produces (RELATIVE, 64,
 * GLOB_DAT, JUMP_SLOT relocations, and DTPMOD64, DTPOFF64 and TPOFF64 for
 * thread-local storage: see ../src/tls.h); no lazy binding.
 *
 * It stays, for dlopen: libvexa's dlopen, dlsym, dlclose and dlerror call
 * __vx_dlopen and the rest here (the loader is the last of the objects
 * symbols are looked up in, so libvexa finds them).
 */
#include <stddef.h>
#include <stdint.h>
#include <vexa/abi.h>
#include "../src/tls.h"

/* ---- ELF ---- */

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_PHDR 6
#define PT_TLS 7
#define PF_X 1
#define PF_W 2
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHNUM 5
#define AT_BASE 7
#define AT_ENTRY 9

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Ehdr;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} Phdr;

typedef struct {
    int64_t tag;
    uint64_t value;
} Dyn;

typedef struct {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
} Sym;

typedef struct {
    uint64_t offset, info;
    int64_t addend;
} Rela;

/* ---- System calls and small helpers ---- */

static long syscall4(long n, long a, long b, long c, long d) {
    long result;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return result;
}

static size_t length(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static int same(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++, b++;
    }
    return *a == *b;
}

static void say(const char *text) {
    syscall4(VX_SYS_WRITE, 2, (long)text, (long)length(text), 0);
}

__attribute__((noreturn)) static void fail(const char *what, const char *detail) {
    say("vexa-ld: ");
    say(what);
    if (detail) {
        say(": ");
        say(detail);
    }
    say("\n");
    syscall4(VX_SYS_EXIT, 127, 0, 0, 0);
    __builtin_unreachable();
}

/* ---- Loaded objects ---- */

#define MAX_OBJECTS 32

struct object {
    uint64_t base; /* Added to every address in the file. */
    Dyn *dynamic;
    Sym *symbols;
    const char *strings;
    uint32_t symbol_count;
    const Phdr *phdrs;
    uint16_t phnum;
    const char *name;
    int references; /* dlopen's. */
    int is_loader;
    char path[128];
};

/* dlopen's errors: kept for dlerror, instead of stopping the program. */
static int soft_errors;
static char error_text[200];

static void set_error(const char *what, const char *detail) {
    size_t n = 0;
    for (const char *p = what; *p && n < sizeof(error_text) - 1; p++) {
        error_text[n++] = *p;
    }
    if (detail) {
        for (const char *p = ": "; *p && n < sizeof(error_text) - 1; p++) {
            error_text[n++] = *p;
        }
        for (const char *p = detail; *p && n < sizeof(error_text) - 1; p++) {
            error_text[n++] = *p;
        }
    }
    error_text[n] = '\0';
}

static uint64_t dyn_value(const struct object *o, int64_t tag) {
    for (Dyn *d = o->dynamic; d && d->tag != DT_NULL; d++) {
        if (d->tag == tag) {
            return d->value;
        }
    }
    return 0;
}

static void read_dynamic(struct object *o) {
    uint64_t hash = dyn_value(o, DT_HASH);
    o->symbols = (Sym *)(o->base + dyn_value(o, DT_SYMTAB));
    o->strings = (const char *)(o->base + dyn_value(o, DT_STRTAB));
    /* DT_HASH is nbucket, nchain, ...: nchain is the number of symbols. */
    o->symbol_count = hash ? ((uint32_t *)(o->base + hash))[1] : 0;
}

/* Relocates the loader itself: until this is done, no pointer stored in its
 * data may be used. */
static void relocate_self(uint64_t base) {
    extern Dyn _DYNAMIC[] __attribute__((visibility("hidden")));
    Rela *rela = 0;
    uint64_t size = 0;
    for (Dyn *d = _DYNAMIC; d->tag != DT_NULL; d++) {
        if (d->tag == DT_RELA) {
            rela = (Rela *)(base + d->value);
        } else if (d->tag == DT_RELASZ) {
            size = d->value;
        }
    }
    for (uint64_t i = 0; rela && i < size / sizeof(Rela); i++) {
        if ((uint32_t)rela[i].info == R_X86_64_RELATIVE) {
            *(uint64_t *)(base + rela[i].offset) = base + rela[i].addend;
        }
    }
}

static long read_at(int fd, void *buffer, uint64_t size, uint64_t offset) {
    if (syscall4(VX_SYS_SEEK, fd, (long)offset, VX_SEEK_SET, 0) < 0) {
        return -1;
    }
    for (uint64_t done = 0; done < size;) {
        long n = syscall4(VX_SYS_READ, fd, (long)((char *)buffer + done), (long)(size - done), 0);
        if (n <= 0) {
            return -1;
        }
        done += (uint64_t)n;
    }
    return 0;
}

/* Library headers are kept here (the loader has no allocator). */
static Phdr phdr_store[MAX_OBJECTS][16];

/* Loads a library (by name from /lib, or a path): 0, or -1 with the error set. */
static int load_library(struct object *o, int index, const char *name) {
    char *path = o->path;
    size_t n = length(name), at = 0;
    int has_slash = 0;
    for (size_t i = 0; i < n; i++) {
        has_slash |= name[i] == '/';
    }
    if (!has_slash) {
        for (const char *p = "/lib/"; *p; p++) {
            path[at++] = *p;
        }
    }
    if (at + n + 1 > sizeof(o->path)) {
        set_error("library name too long", name);
        return -1;
    }
    for (size_t i = 0; i <= n; i++) {
        path[at + i] = name[i];
    }
    int fd = (int)syscall4(VX_SYS_OPEN, (long)path, (long)length(path), VX_OPEN_READ, 0);
    if (fd < 0) {
        set_error("library not found", path);
        return -1;
    }
    Ehdr header;
    Phdr *phdrs = phdr_store[index];
    if (read_at(fd, &header, sizeof(header), 0) || header.phnum > 16 ||
        header.phentsize != sizeof(Phdr) ||
        read_at(fd, phdrs, header.phnum * sizeof(Phdr), header.phoff)) {
        syscall4(VX_SYS_CLOSE, fd, 0, 0, 0);
        set_error("not a library", path);
        return -1;
    }
    uint64_t low = ~0ULL, high = 0;
    for (int i = 0; i < header.phnum; i++) {
        if (phdrs[i].type == PT_LOAD) {
            uint64_t start = phdrs[i].vaddr & ~0xfffULL;
            uint64_t end = (phdrs[i].vaddr + phdrs[i].memsz + 0xfff) & ~0xfffULL;
            low = start < low ? start : low;
            high = end > high ? end : high;
        }
    }
    long mapped = low < high ? syscall4(VX_SYS_MAP, (long)(high - low), VX_MAP_WRITE, 0, 0) : -1;
    if (mapped < 0) {
        syscall4(VX_SYS_CLOSE, fd, 0, 0, 0);
        set_error(low < high ? "out of memory loading" : "nothing to load in", path);
        return -1;
    }
    o->base = (uint64_t)mapped - low;
    o->dynamic = 0;
    for (int i = 0; i < header.phnum; i++) {
        if (phdrs[i].type == PT_LOAD &&
            read_at(fd, (void *)(o->base + phdrs[i].vaddr), phdrs[i].filesz, phdrs[i].offset)) {
            syscall4(VX_SYS_CLOSE, fd, 0, 0, 0);
            set_error("can't read", path);
            return -1;
        }
        if (phdrs[i].type == PT_DYNAMIC) {
            o->dynamic = (Dyn *)(o->base + phdrs[i].vaddr);
        }
    }
    syscall4(VX_SYS_CLOSE, fd, 0, 0, 0);
    if (!o->dynamic) {
        set_error("not a shared library", path);
        return -1;
    }
    o->phdrs = phdrs;
    o->phnum = header.phnum;
    o->name = path;
    o->references = 1;
    o->is_loader = 0;
    read_dynamic(o);
    return 0;
}

static struct object objects[MAX_OBJECTS];
static int object_count;

/* The first definition of `name` (the program's, then the libraries'), and
 * which object has it; 0 if there's none (an error unless `weak`). */
static const Sym *find(const char *name, int weak, const char *user, const struct object **owner) {
    for (int i = 0; i < object_count; i++) {
        const struct object *o = &objects[i];
        for (uint32_t s = 1; s < o->symbol_count; s++) {
            const Sym *sym = &o->symbols[s];
            if (sym->shndx != 0 && (sym->info >> 4) != 0 /* not local */ &&
                same(o->strings + sym->name, name)) {
                *owner = o;
                return sym;
            }
        }
    }
    *owner = 0;
    if (!weak) {
        if (soft_errors) {
            set_error("undefined symbol", name);
            soft_errors = 2; /* (Something wasn't found.) */
            return 0;
        }
        say("vexa-ld: ");
        say(user);
        fail(": undefined symbol", name);
    }
    return 0;
}

/* The address of the first definition of `name`. */
static uint64_t lookup(const char *name, int weak, const char *user) {
    const struct object *owner;
    const Sym *sym = find(name, weak, user, &owner);
    return sym ? owner->base + sym->value : 0;
}

/* ---- Thread-local storage ---- */

static struct vx_tls_table tls;

/* Notes an object's PT_TLS, as module `index` + 1: with a static offset
 * while the program is starting (`at_start`), dynamic for dlopen's. */
static void note_tls(const struct object *o, int index, int at_start) {
    struct vx_tls_module *m = &tls.modules[index + 1];
    *m = (struct vx_tls_module){0};
    for (int i = 0; i < o->phnum; i++) {
        const Phdr *ph = &o->phdrs[i];
        if (ph->type != PT_TLS || ph->memsz == 0) {
            continue;
        }
        m->image = ph->filesz ? (const void *)(o->base + ph->vaddr) : 0;
        m->filesz = ph->filesz;
        m->memsz = ph->memsz;
        m->align = ph->align ? ph->align : 1;
        if (at_start) {
            uint64_t end = tls.static_size + m->memsz;
            m->offset = (end + m->align - 1) & ~(m->align - 1);
            tls.static_size = m->offset;
            tls.static_align = m->align > tls.static_align ? m->align : tls.static_align;
        }
    }
}

/* For libvexa: every module's TLS (see ../src/tls.h). */
__attribute__((visibility("default"))) const struct vx_tls_table *__vx_tls_table(void) {
    return &tls;
}

static void relocate(const struct object *o, Rela *rela, uint64_t size) {
    for (uint64_t i = 0; rela && i < size / sizeof(Rela); i++) {
        uint32_t type = (uint32_t)rela[i].info;
        uint32_t index = (uint32_t)(rela[i].info >> 32);
        uint64_t *where = (uint64_t *)(o->base + rela[i].offset);
        uint64_t symbol = 0;
        const Sym *found = 0;
        const struct object *owner = o; /* (TLS without a symbol: the object's own.) */
        if (index) {
            const Sym *sym = &o->symbols[index];
            found = find(o->strings + sym->name, (sym->info >> 4) == 2 /* weak */, o->name, &owner);
            symbol = found ? owner->base + found->value : 0;
        }
        /* TLS symbols' values are offsets in their module's block. */
        uint64_t tls_value = (found ? found->value : 0) + rela[i].addend;
        const struct vx_tls_module *module = owner ? &tls.modules[owner - objects + 1] : 0;
        switch (type) {
        case R_X86_64_RELATIVE: *where = o->base + rela[i].addend; break;
        case R_X86_64_64: *where = symbol + rela[i].addend; break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: *where = symbol; break;
        case R_X86_64_DTPMOD64: *where = owner ? (uint64_t)(owner - objects + 1) : 0; break;
        case R_X86_64_DTPOFF64: *where = tls_value; break;
        case R_X86_64_TPOFF64:
            if (module && module->memsz && module->offset) {
                *where = tls_value - module->offset;
                break;
            }
            if (soft_errors) {
                set_error("uses static thread-local storage (can't be dlopened)", o->name);
                soft_errors = 2;
                return;
            }
            fail("thread-local storage it can't reach in", o->name);
        default:
            if (soft_errors) {
                set_error("unsupported relocation in", o->name);
                soft_errors = 2;
                return;
            }
            fail("unsupported relocation in", o->name);
        }
    }
}

/* Final permissions: code read-only and executable, data writable. */
static void protect(const struct object *o) {
    for (int i = 0; i < o->phnum; i++) {
        const Phdr *ph = &o->phdrs[i];
        if (ph->type != PT_LOAD) {
            continue;
        }
        uint64_t start = (o->base + ph->vaddr) & ~0xfffULL;
        uint64_t end = (o->base + ph->vaddr + ph->memsz + 0xfff) & ~0xfffULL;
        unsigned flags = (ph->flags & PF_W ? VX_MAP_WRITE : 0) | (ph->flags & PF_X ? VX_MAP_EXEC : 0);
        syscall4(VX_SYS_PROTECT, (long)start, (long)(end - start), flags, 0);
    }
}

uint64_t loader_main(uint64_t *stack) {
    /* Find the auxiliary vector: after argv and envp. */
    uint64_t argc = stack[0];
    uint64_t *p = stack + 1 + argc + 1;
    while (*p) {
        p++;
    }
    uint64_t *aux = p + 1;
    uint64_t phdr = 0, phnum = 0, base = 0, entry = 0;
    for (uint64_t *a = aux; a[0] != AT_NULL; a += 2) {
        switch (a[0]) {
        case AT_PHDR: phdr = a[1]; break;
        case AT_PHNUM: phnum = a[1]; break;
        case AT_BASE: base = a[1]; break;
        case AT_ENTRY: entry = a[1]; break;
        }
    }
    relocate_self(base);

    /* The program, as the kernel loaded it. */
    struct object *program = &objects[object_count++];
    program->phdrs = (const Phdr *)phdr;
    program->phnum = (uint16_t)phnum;
    program->name = "the program";
    for (uint64_t i = 0; i < phnum; i++) {
        if (program->phdrs[i].type == PT_PHDR) {
            program->base = phdr - program->phdrs[i].vaddr;
        }
    }
    for (uint64_t i = 0; i < phnum; i++) {
        if (program->phdrs[i].type == PT_DYNAMIC) {
            program->dynamic = (Dyn *)(program->base + program->phdrs[i].vaddr);
        }
    }
    if (!program->dynamic) {
        fail("the program isn't dynamically linked", 0);
    }
    read_dynamic(program);

    /* Its libraries (not theirs in turn: libvexa needs none). */
    for (Dyn *d = program->dynamic; d->tag != DT_NULL; d++) {
        if (d->tag == DT_NEEDED) {
            if (object_count == MAX_OBJECTS - 1) {
                fail("too many libraries", 0);
            }
            if (load_library(&objects[object_count], object_count, program->strings + d->value)) {
                fail(error_text, 0);
            }
            object_count++;
        }
    }
    /* The loader itself, last: what it offers libvexa (dlopen). */
    struct object *loader = &objects[object_count++];
    extern Dyn _DYNAMIC[] __attribute__((visibility("hidden")));
    loader->base = base;
    loader->dynamic = _DYNAMIC;
    loader->name = "vexa-ld.so";
    loader->is_loader = 1;
    read_dynamic(loader);

    /* Thread-local storage: the program's and its libraries', static. */
    for (int i = 0; i < object_count; i++) {
        if (!objects[i].is_loader) {
            note_tls(&objects[i], i, 1);
        }
    }

    /* Libraries first, so their data is ready; then the program. */
    for (int i = object_count - 1; i >= 0; i--) {
        struct object *o = &objects[i];
        if (!o->is_loader) {
            relocate(o, (Rela *)(o->base + dyn_value(o, DT_RELA)), dyn_value(o, DT_RELASZ));
            relocate(o, (Rela *)(o->base + dyn_value(o, DT_JMPREL)), dyn_value(o, DT_PLTRELSZ));
        }
    }
    for (int i = 1; i < object_count; i++) {
        if (!objects[i].is_loader) {
            protect(&objects[i]);
        }
    }
    for (int i = object_count - 1; i >= 1; i--) {
        const struct object *o = &objects[i];
        uint64_t array = dyn_value(o, DT_INIT_ARRAY), size = dyn_value(o, DT_INIT_ARRAYSZ);
        for (uint64_t j = 0; !o->is_loader && array && j < size / sizeof(uint64_t); j++) {
            ((void (*)(void))((uint64_t *)(o->base + array))[j])();
        }
    }
    return entry;
}

/* ---- dlopen and the rest (called by libvexa) ---- */

#define EXPORT __attribute__((visibility("default")))

static const char *base_name(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            base = p + 1;
        }
    }
    return base;
}

static struct object *loaded(const char *file) {
    for (int i = 1; i < object_count; i++) {
        struct object *o = &objects[i];
        if (!o->is_loader && (same(o->name, file) || same(base_name(o->name), file))) {
            return o;
        }
    }
    return 0;
}

EXPORT void *__vx_dlopen(const char *file, int flags) {
    (void)flags; /* (Everything is bound at once, and visible to everything.) */
    if (!file) {
        return &objects[0]; /* The program (and everything it has). */
    }
    struct object *already = loaded(file);
    if (already) {
        already->references++;
        return already;
    }
    int first = object_count;
    soft_errors = 1;
    /* It, then what it needs that isn't here yet (and so on). */
    if (object_count == MAX_OBJECTS || load_library(&objects[object_count], object_count, file)) {
        if (object_count == MAX_OBJECTS) {
            set_error("too many libraries", file);
        }
        soft_errors = 0;
        return 0;
    }
    object_count++;
    for (int i = first; i < object_count; i++) {
        for (Dyn *d = objects[i].dynamic; d->tag != DT_NULL; d++) {
            const char *needed = objects[i].strings + d->value;
            if (d->tag != DT_NEEDED || loaded(needed)) {
                continue;
            }
            if (object_count == MAX_OBJECTS ||
                load_library(&objects[object_count], object_count, needed)) {
                object_count = first;
                soft_errors = 0;
                return 0;
            }
            object_count++;
        }
    }
    for (int i = first; i < object_count; i++) {
        note_tls(&objects[i], i, 0);
    }
    for (int i = object_count - 1; i >= first && soft_errors == 1; i--) {
        struct object *o = &objects[i];
        relocate(o, (Rela *)(o->base + dyn_value(o, DT_RELA)), dyn_value(o, DT_RELASZ));
        relocate(o, (Rela *)(o->base + dyn_value(o, DT_JMPREL)), dyn_value(o, DT_PLTRELSZ));
    }
    if (soft_errors != 1) {
        object_count = first; /* (Its memory stays; it isn't used.) */
        soft_errors = 0;
        return 0;
    }
    soft_errors = 0;
    for (int i = first; i < object_count; i++) {
        protect(&objects[i]);
    }
    for (int i = object_count - 1; i >= first; i--) {
        const struct object *o = &objects[i];
        uint64_t array = dyn_value(o, DT_INIT_ARRAY), size = dyn_value(o, DT_INIT_ARRAYSZ);
        for (uint64_t j = 0; array && j < size / sizeof(uint64_t); j++) {
            ((void (*)(void))((uint64_t *)(o->base + array))[j])();
        }
    }
    return &objects[first];
}

EXPORT void *__vx_dlsym(void *handle, const char *name) {
    struct object *only = handle && handle != &objects[0] ? handle : 0;
    if (only) {
        for (uint32_t s = 1; s < only->symbol_count; s++) {
            const Sym *sym = &only->symbols[s];
            if (sym->shndx != 0 && (sym->info >> 4) != 0 && same(only->strings + sym->name, name)) {
                return (void *)(only->base + sym->value);
            }
        }
    }
    /* (Then, or for the program, everything loaded.) */
    uint64_t address = lookup(name, 1, 0);
    if (!address) {
        set_error("undefined symbol", name);
    }
    return (void *)address;
}

EXPORT int __vx_dlclose(void *handle) {
    struct object *o = handle;
    if (o && o != &objects[0] && o->references > 0) {
        o->references--; /* (Libraries stay loaded: other code may point into them.) */
    }
    return 0;
}

EXPORT const char *__vx_dlerror(void) {
    if (!error_text[0]) {
        return 0;
    }
    static char last[sizeof(error_text)];
    for (size_t i = 0; i < sizeof(error_text); i++) {
        last[i] = error_text[i];
    }
    error_text[0] = '\0';
    return last;
}

/* The compiler may call these even here (for large copies and zeroing). */
__attribute__((visibility("hidden"))) void *memcpy(void *to, const void *from, size_t n) {
    char *d = to;
    const char *s = from;
    while (n--) {
        *d++ = *s++;
    }
    return to;
}

__attribute__((visibility("hidden"))) void *memset(void *to, int value, size_t n) {
    char *d = to;
    while (n--) {
        *d++ = (char)value;
    }
    return to;
}
