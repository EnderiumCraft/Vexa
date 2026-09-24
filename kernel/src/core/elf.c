#include <stdbool.h>
#include <vexa/abi.h>
#include <vexa/elf.h>
#include <vexa/mm.h>
#include <vexa/process.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_NOTE 4
#define PF_X 1
#define PF_W 2

struct elf64_header {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

struct elf64_note {
    uint32_t namesz;
    uint32_t descsz;
    uint32_t type;
};

static bool in_file(uint64_t offset, uint64_t length, size_t size) {
    return offset <= size && length <= size - offset;
}

/* True if a PT_NOTE segment contains the Vexa ABI note. */
static bool has_vexa_note(const uint8_t *data, const struct elf64_phdr *ph) {
    uint64_t pos = ph->offset, end = ph->offset + ph->filesz;
    while (pos + sizeof(struct elf64_note) <= end) {
        const struct elf64_note *note = (const void *)(data + pos);
        uint64_t name_at = pos + sizeof(*note);
        uint64_t desc_at = name_at + ((note->namesz + 3) & ~3U);
        uint64_t next = desc_at + ((note->descsz + 3) & ~3U);
        if (next > end) {
            break;
        }
        if (note->type == VX_NOTE_TYPE_ABI && note->namesz == sizeof(VX_NOTE_NAME) &&
            memcmp(data + name_at, VX_NOTE_NAME, sizeof(VX_NOTE_NAME)) == 0) {
            return true;
        }
        pos = next;
    }
    return false;
}

static int load_segment(struct address_space *as, const uint8_t *data,
                        const struct elf64_phdr *ph) {
    unsigned flags = (ph->flags & PF_W ? VMM_WRITE : 0) | (ph->flags & PF_X ? VMM_EXEC : 0);
    for (uint64_t page = ph->vaddr & ~(PAGE_SIZE - 1); page < ph->vaddr + ph->memsz;
         page += PAGE_SIZE) {
        uint64_t phys = vmm_map_user_page(as, page, flags);
        if (!phys) {
            return -1;
        }
        /* Copy the part of the file that lands in this page, through the
         * direct map (the address space isn't active yet). */
        uint64_t from = page > ph->vaddr ? page : ph->vaddr;
        uint64_t to = page + PAGE_SIZE;
        if (to > ph->vaddr + ph->filesz) {
            to = ph->vaddr + ph->filesz;
        }
        if (from < to) {
            memcpy((uint8_t *)phys_to_virt(phys) + (from - page),
                   data + ph->offset + (from - ph->vaddr), to - from);
        }
    }
    return 0;
}

int elf_load(struct address_space *as, const void *elf, size_t size, uint64_t *entry,
             const struct personality **personality, const char **reason) {
    const uint8_t *data = elf;
    const struct elf64_header *header = elf;
    if (size < sizeof(*header) || memcmp(header->ident, "\x7f" "ELF", 4) != 0) {
        *reason = "not an ELF file";
        return -1;
    }
    if (header->ident[4] != ELFCLASS64 || header->ident[5] != ELFDATA2LSB ||
        header->machine != EM_X86_64) {
        *reason = "not an x86_64 program";
        return -1;
    }
    if (header->type != ET_EXEC) {
        *reason = "only statically linked executables are supported so far";
        return -1;
    }
    if (header->phentsize != sizeof(struct elf64_phdr) ||
        !in_file(header->phoff, (uint64_t)header->phnum * sizeof(struct elf64_phdr), size)) {
        *reason = "corrupt program headers";
        return -1;
    }
    const struct elf64_phdr *phdrs = (const void *)(data + header->phoff);

    bool native = false;
    for (int i = 0; i < header->phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];
        if (ph->type == PT_NOTE && in_file(ph->offset, ph->filesz, size) &&
            has_vexa_note(data, ph)) {
            native = true;
        }
    }
    if (!native) {
        *reason = "not a Vexa program (Linux programs need the Linux subsystem, due in Phase 5)";
        return -1;
    }

    for (int i = 0; i < header->phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];
        if (ph->type != PT_LOAD || ph->memsz == 0) {
            continue;
        }
        if (ph->filesz > ph->memsz || !in_file(ph->offset, ph->filesz, size) ||
            ph->vaddr < USER_BASE || ph->vaddr + ph->memsz < ph->vaddr ||
            ph->vaddr + ph->memsz > USER_END) {
            *reason = "a segment is outside the file or outside user memory";
            return -1;
        }
        if (load_segment(as, data, ph) != 0) {
            *reason = "out of memory";
            return -1;
        }
    }
    if (header->entry < USER_BASE || header->entry >= USER_END) {
        *reason = "entry point is outside user memory";
        return -1;
    }
    *entry = header->entry;
    *personality = &vexa_personality;
    return 0;
}
