/* Thread-local storage (see tls.h): each thread's block holds its static
 * TLS just below its thread record, and __tls_get_addr finds (or makes, on
 * first use) a module's TLS for code that asks for it by module. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vexa/syscall.h>
#include "internal.h"
#include "tls.h"

/* The loader's table, for programs that have one (weak: static ones don't,
 * and find their own PT_TLS through their ELF header). */
extern const struct vx_tls_table *__vx_tls_table(void) __attribute__((weak));
extern const char __ehdr_start[] __attribute__((weak, visibility("hidden")));

static struct vx_tls_table own;
static const struct vx_tls_table *table = &own;

static uint64_t round_up(uint64_t n, uint64_t align) {
    return (n + align - 1) & ~(align - 1);
}

void __libvexa_tls_init(void) {
    if (__vx_tls_table) {
        table = __vx_tls_table();
        return;
    }
    if (!__ehdr_start) {
        return;
    }
    /* A static program: Elf64_Ehdr's e_phoff is at 32, e_phnum at 56. */
    uint64_t phoff;
    uint16_t phnum;
    memcpy(&phoff, __ehdr_start + 32, sizeof(phoff));
    memcpy(&phnum, __ehdr_start + 56, sizeof(phnum));
    struct phdr {
        uint32_t type, flags;
        uint64_t offset, vaddr, paddr, filesz, memsz, align;
    };
    const struct phdr *ph = (const struct phdr *)(__ehdr_start + phoff);
    uintptr_t base = 0;
    for (unsigned i = 0; i < phnum; i++) {
        if (ph[i].type == 1 /* PT_LOAD */ && ph[i].offset == 0) {
            base = (uintptr_t)__ehdr_start - ph[i].vaddr;
        }
    }
    for (unsigned i = 0; i < phnum; i++) {
        if (ph[i].type == 7 /* PT_TLS */ && ph[i].memsz) {
            struct vx_tls_module *m = &own.modules[1];
            m->image = ph[i].filesz ? (const void *)(base + ph[i].vaddr) : NULL;
            m->filesz = ph[i].filesz;
            m->memsz = ph[i].memsz;
            m->align = ph[i].align ? ph[i].align : 1;
            m->offset = round_up(m->memsz, m->align);
            own.static_size = m->offset;
            own.static_align = m->align;
        }
    }
}

/* A thread's block: its static TLS, then its record (`size` bytes, starting
 * with its struct __vx_tcb, where the thread pointer points). Zeroed. */
struct __vx_tcb *__libvexa_tcb_alloc(size_t size) {
    uint64_t align = table->static_align > 64 ? table->static_align : 64;
    uint64_t below = round_up(table->static_size, align);
    uint64_t total = round_up(below + size, 4096);
    char *base = vx_map(total, VX_MAP_WRITE);
    if (!base) {
        return NULL;
    }
    struct __vx_tcb *tcb = (struct __vx_tcb *)(base + below);
    tcb->self = tcb;
    tcb->block = base;
    tcb->block_size = total;
    for (int i = 1; i < VX_TLS_MODULES; i++) {
        const struct vx_tls_module *m = &table->modules[i];
        if (m->memsz && m->offset) {
            char *tls = (char *)tcb - m->offset;
            if (m->image) {
                memcpy(tls, m->image, m->filesz);
            }
            tcb->dtv[i] = tls;
        }
    }
    return tcb;
}

void __libvexa_tcb_free(struct __vx_tcb *tcb) {
    if (!tcb) {
        return;
    }
    for (int i = 1; i < VX_TLS_MODULES; i++) {
        if (tcb->dtv[i] && !table->modules[i].offset) {
            free(tcb->dtv[i]); /* (A dlopened module's, made on first use.) */
        }
    }
    vx_unmap(tcb->block, tcb->block_size);
}

struct tls_index {
    unsigned long module, offset;
};

/* What code compiled for shared libraries calls for its thread-locals. */
void *__tls_get_addr(struct tls_index *index) {
    struct __vx_tcb *tcb = __vx_tcb();
    if (index->module == 0 || index->module >= VX_TLS_MODULES) {
        abort();
    }
    char *tls = tcb->dtv[index->module];
    if (!tls) {
        const struct vx_tls_module *m = &table->modules[index->module];
        uint64_t align = m->align > 16 ? m->align : 16;
        tls = aligned_alloc(align, round_up(m->memsz ? m->memsz : 1, align));
        if (!tls) {
            abort();
        }
        if (m->image) {
            memcpy(tls, m->image, m->filesz);
        }
        memset(tls + m->filesz, 0, m->memsz - m->filesz);
        tcb->dtv[index->module] = tls;
    }
    return tls + index->offset;
}
