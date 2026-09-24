#include <stdbool.h>
#include <vexa/abi.h>
#include <vexa/elf.h>
#include <vexa/mm.h>
#include <vexa/process.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>
#include <vexa/vfs.h>

#define ELFCLASS64 2
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_X86_64 62
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_PHDR 6
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

#define MAX_PHDRS 64
#define MAX_NOTE_SIZE 4096

static bool in_file(uint64_t offset, uint64_t length, uint64_t size) {
    return offset <= size && length <= size - offset;
}

static bool read_exact(struct file *file, void *buffer, uint64_t size, uint64_t offset) {
    return vfs_pread(file, buffer, size, offset) == (int64_t)size;
}

/* True if a PT_NOTE segment contains the Vexa ABI note. */
static bool has_vexa_note(struct file *file, const struct elf64_phdr *ph) {
    if (ph->filesz > MAX_NOTE_SIZE) {
        return false;
    }
    uint8_t *notes = kmalloc(ph->filesz);
    bool found = false;
    if (notes && read_exact(file, notes, ph->filesz, ph->offset)) {
        uint64_t pos = 0, end = ph->filesz;
        while (!found && pos + sizeof(struct elf64_note) <= end) {
            const struct elf64_note *note = (const void *)(notes + pos);
            uint64_t name_at = pos + sizeof(*note);
            uint64_t desc_at = name_at + ((note->namesz + 3) & ~3U);
            uint64_t next = desc_at + ((note->descsz + 3) & ~3U);
            if (next > end) {
                break;
            }
            found = note->type == VX_NOTE_TYPE_ABI && note->namesz == sizeof(VX_NOTE_NAME) &&
                    memcmp(notes + name_at, VX_NOTE_NAME, sizeof(VX_NOTE_NAME)) == 0;
            pos = next;
        }
    }
    kfree(notes);
    return found;
}

static int load_segment(struct address_space *as, struct file *file,
                        const struct elf64_phdr *ph, uint64_t base) {
    unsigned flags = (ph->flags & PF_W ? VM_WRITE : 0) | (ph->flags & PF_X ? VM_EXEC : 0);
    uint64_t vaddr = ph->vaddr + base;
    if (vm_add_area(as, vaddr, vaddr + ph->memsz, flags)) {
        return -1;
    }
    /* Copy the file's part of the segment page by page, straight into the
     * pages through the direct map (the address space isn't active yet). The
     * rest (.bss) is zero-filled on first touch. */
    for (uint64_t page = vaddr & ~(PAGE_SIZE - 1); page < vaddr + ph->filesz; page += PAGE_SIZE) {
        /* Not "for writing": read-only segments are written through the direct
         * map, which ignores the user permissions. */
        uint64_t phys = vm_page_for(as, page, false);
        if (!phys) {
            return -1;
        }
        uint64_t from = page > vaddr ? page : vaddr;
        uint64_t to = page + PAGE_SIZE < vaddr + ph->filesz ? page + PAGE_SIZE : vaddr + ph->filesz;
        if (!read_exact(file, (uint8_t *)phys_to_virt(phys) + (from - page), to - from,
                        ph->offset + (from - vaddr))) {
            return -1;
        }
    }
    return 0;
}

/* Position-independent executables (ET_DYN) go here, like on Linux. */
#define PIE_BASE 0x0000555555554000ULL

int elf_load(struct address_space *as, struct file *file, struct elf_image *image,
             const char **reason) {
    struct vx_stat stat;
    vfs_file_stat(file, &stat);
    uint64_t size = stat.size;
    struct elf64_header header;
    if (!read_exact(file, &header, sizeof(header), 0) ||
        memcmp(header.ident, "\x7f" "ELF", 4) != 0) {
        *reason = "not a program (no ELF header)";
        return -VX_ENOEXEC;
    }
    if (header.ident[4] != ELFCLASS64 || header.ident[5] != ELFDATA2LSB ||
        header.machine != EM_X86_64) {
        *reason = "not an x86_64 program";
        return -VX_ENOEXEC;
    }
    if (header.type != ET_EXEC && header.type != ET_DYN) {
        *reason = "not an executable";
        return -VX_ENOEXEC;
    }
    if (header.phentsize != sizeof(struct elf64_phdr) || header.phnum > MAX_PHDRS ||
        !in_file(header.phoff, (uint64_t)header.phnum * sizeof(struct elf64_phdr), size)) {
        *reason = "corrupt program headers";
        return -VX_ENOEXEC;
    }
    struct elf64_phdr *phdrs = kmalloc(header.phnum * sizeof(struct elf64_phdr) + 1);
    if (!phdrs || !read_exact(file, phdrs, header.phnum * sizeof(struct elf64_phdr), header.phoff)) {
        kfree(phdrs);
        *reason = "could not read the program headers";
        return -VX_EIO;
    }

    int result = -VX_ENOEXEC;
    bool native = false;
    uint64_t base = header.type == ET_DYN ? PIE_BASE : 0;
    uint64_t phdr_address = 0, highest = 0;
    for (int i = 0; i < header.phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];
        if (ph->type == PT_NOTE && in_file(ph->offset, ph->filesz, size) && has_vexa_note(file, ph)) {
            native = true;
        } else if (ph->type == PT_INTERP) {
            *reason = "dynamically linked (Vexa can run static programs only, until Phase 6)";
            goto out;
        } else if (ph->type == PT_PHDR) {
            phdr_address = ph->vaddr + base;
        }
    }
    image->personality = native ? &vexa_personality : NULL;
#ifdef LINUX_COMPAT
    if (!native) {
        image->personality = &linux_personality;
    }
#endif
    if (!image->personality) {
        *reason = "not a Vexa program (this kernel was built without the Linux subsystem)";
        goto out;
    }

    for (int i = 0; i < header.phnum; i++) {
        const struct elf64_phdr *ph = &phdrs[i];
        if (ph->type != PT_LOAD || ph->memsz == 0) {
            continue;
        }
        uint64_t vaddr = ph->vaddr + base;
        if (ph->filesz > ph->memsz || !in_file(ph->offset, ph->filesz, size) ||
            vaddr < USER_BASE || vaddr + ph->memsz < vaddr || vaddr + ph->memsz > USER_END) {
            *reason = "a segment is outside the file or outside user memory";
            goto out;
        }
        if (load_segment(as, file, ph, base) != 0) {
            *reason = "out of memory, or the file could not be read";
            result = -VX_ENOMEM;
            goto out;
        }
        /* The program headers are in memory if a segment covers them. */
        if (!phdr_address && ph->offset <= header.phoff &&
            header.phoff + header.phnum * sizeof(struct elf64_phdr) <= ph->offset + ph->filesz) {
            phdr_address = vaddr + (header.phoff - ph->offset);
        }
        if (vaddr + ph->memsz > highest) {
            highest = vaddr + ph->memsz;
        }
    }
    uint64_t entry = header.entry + base;
    if (entry < USER_BASE || entry >= USER_END || !highest) {
        *reason = "entry point is outside user memory";
        goto out;
    }
    image->entry = entry;
    image->phdr_address = phdr_address;
    image->phent = sizeof(struct elf64_phdr);
    image->phnum = header.phnum;
    image->heap_start = (highest + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    result = 0;
out:
    kfree(phdrs);
    return result;
}
