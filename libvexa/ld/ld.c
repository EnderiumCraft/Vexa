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
 * GLOB_DAT, JUMP_SLOT relocations); no thread-local storage or lazy binding.
 */
#include <stddef.h>
#include <stdint.h>
#include <vexa/abi.h>

/* ---- ELF ---- */

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_PHDR 6
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

#define MAX_OBJECTS 16

struct object {
    uint64_t base; /* Added to every address in the file. */
    Dyn *dynamic;
    Sym *symbols;
    const char *strings;
    uint32_t symbol_count;
    const Phdr *phdrs;
    uint16_t phnum;
    const char *name;
};

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

static void load_library(struct object *o, int index, const char *name) {
    char path[256] = "/lib/";
    size_t n = length(name);
    if (n + 6 > sizeof(path)) {
        fail("library name too long", name);
    }
    for (size_t i = 0; i <= n; i++) {
        path[5 + i] = name[i];
    }
    int fd = (int)syscall4(VX_SYS_OPEN, (long)path, (long)length(path), VX_OPEN_READ, 0);
    if (fd < 0) {
        fail("library not found", path);
    }
    Ehdr header;
    if (read_at(fd, &header, sizeof(header), 0) || header.phnum > 16 ||
        header.phentsize != sizeof(Phdr)) {
        fail("not a library", path);
    }
    Phdr *phdrs = phdr_store[index];
    if (read_at(fd, phdrs, header.phnum * sizeof(Phdr), header.phoff)) {
        fail("can't read", path);
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
    if (low >= high) {
        fail("nothing to load in", path);
    }
    long mapped = syscall4(VX_SYS_MAP, (long)(high - low), VX_MAP_WRITE, 0, 0);
    if (mapped < 0) {
        fail("out of memory loading", path);
    }
    o->base = (uint64_t)mapped - low;
    for (int i = 0; i < header.phnum; i++) {
        if (phdrs[i].type == PT_LOAD &&
            read_at(fd, (void *)(o->base + phdrs[i].vaddr), phdrs[i].filesz, phdrs[i].offset)) {
            fail("can't read", path);
        }
        if (phdrs[i].type == PT_DYNAMIC) {
            o->dynamic = (Dyn *)(o->base + phdrs[i].vaddr);
        }
    }
    syscall4(VX_SYS_CLOSE, fd, 0, 0, 0);
    o->phdrs = phdrs;
    o->phnum = header.phnum;
    o->name = name;
    read_dynamic(o);
}

static struct object objects[MAX_OBJECTS];
static int object_count;

/* The address of the first definition of `name`: the program, then libraries. */
static uint64_t lookup(const char *name, int weak, const char *user) {
    for (int i = 0; i < object_count; i++) {
        const struct object *o = &objects[i];
        for (uint32_t s = 1; s < o->symbol_count; s++) {
            const Sym *sym = &o->symbols[s];
            if (sym->shndx != 0 && (sym->info >> 4) != 0 /* not local */ &&
                same(o->strings + sym->name, name)) {
                return o->base + sym->value;
            }
        }
    }
    if (!weak) {
        say("vexa-ld: ");
        say(user);
        fail(": undefined symbol", name);
    }
    return 0;
}

static void relocate(const struct object *o, Rela *rela, uint64_t size) {
    for (uint64_t i = 0; rela && i < size / sizeof(Rela); i++) {
        uint32_t type = (uint32_t)rela[i].info;
        uint32_t index = (uint32_t)(rela[i].info >> 32);
        uint64_t *where = (uint64_t *)(o->base + rela[i].offset);
        uint64_t symbol = 0;
        if (index) {
            const Sym *sym = &o->symbols[index];
            symbol = lookup(o->strings + sym->name, (sym->info >> 4) == 2 /* weak */, o->name);
        }
        switch (type) {
        case R_X86_64_RELATIVE: *where = o->base + rela[i].addend; break;
        case R_X86_64_64: *where = symbol + rela[i].addend; break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: *where = symbol; break;
        default: fail("unsupported relocation in", o->name);
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
            if (object_count == MAX_OBJECTS) {
                fail("too many libraries", 0);
            }
            load_library(&objects[object_count], object_count, program->strings + d->value);
            object_count++;
        }
    }

    /* Libraries first, so their data is ready; then the program. */
    for (int i = object_count - 1; i >= 0; i--) {
        struct object *o = &objects[i];
        relocate(o, (Rela *)(o->base + dyn_value(o, DT_RELA)), dyn_value(o, DT_RELASZ));
        relocate(o, (Rela *)(o->base + dyn_value(o, DT_JMPREL)), dyn_value(o, DT_PLTRELSZ));
    }
    for (int i = 1; i < object_count; i++) {
        protect(&objects[i]);
    }
    for (int i = object_count - 1; i >= 1; i--) {
        const struct object *o = &objects[i];
        uint64_t array = dyn_value(o, DT_INIT_ARRAY), size = dyn_value(o, DT_INIT_ARRAYSZ);
        for (uint64_t j = 0; array && j < size / sizeof(uint64_t); j++) {
            ((void (*)(void))((uint64_t *)(o->base + array))[j])();
        }
    }
    return entry;
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
