#include <vexa/mm.h>
#include <vexa/process.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>

/* Checks a user range and makes every page in it present (and writable, for
 * writes), so the copy that follows can't fault. Processes are single-threaded
 * for now, so nothing can unmap the range between here and the copy;
 * multi-threaded processes (Phase 6) will need the copy itself to recover from
 * faults instead. */
static bool prepare(uint64_t address, size_t size, bool write) {
    struct process *process = process_current();
    if (!process || !process->address_space || address < USER_BASE ||
        address + size < address || address + size > USER_END) {
        return false;
    }
    for (uint64_t page = address & ~(PAGE_SIZE - 1); page < address + size; page += PAGE_SIZE) {
        if (!vm_page_for(process->address_space, page, write)) {
            return false;
        }
    }
    return true;
}

bool copy_from_user(void *dest, uint64_t user_src, size_t size) {
    if (size == 0) {
        return true;
    }
    if (!prepare(user_src, size, false)) {
        return false;
    }
    user_access_begin();
    memcpy(dest, (const void *)user_src, size);
    user_access_end();
    return true;
}

bool copy_to_user(uint64_t user_dest, const void *src, size_t size) {
    if (size == 0) {
        return true;
    }
    if (!prepare(user_dest, size, true)) {
        return false;
    }
    user_access_begin();
    memcpy((void *)user_dest, src, size);
    user_access_end();
    return true;
}

int64_t copy_string_from_user(char *dest, uint64_t user_src, size_t max) {
    for (size_t i = 0; i < max; i++) {
        if (!copy_from_user(dest + i, user_src + i, 1)) {
            return -1;
        }
        if (dest[i] == '\0') {
            return (int64_t)i;
        }
    }
    return -1; /* Too long. */
}
