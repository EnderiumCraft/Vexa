#include <vexa/mm.h>
#include <vexa/string.h>
#include <vexa/uaccess.h>

/* Processes are single-threaded for now, so nothing can unmap the range
 * between the check and the copy. Multi-threaded processes (Phase 6) will need
 * the copy itself to recover from page faults instead. */

bool copy_from_user(void *dest, uint64_t user_src, size_t size) {
    if (!vmm_user_range_mapped(user_src, size, false)) {
        return false;
    }
    user_access_begin();
    memcpy(dest, (const void *)user_src, size);
    user_access_end();
    return true;
}

bool copy_to_user(uint64_t user_dest, const void *src, size_t size) {
    if (!vmm_user_range_mapped(user_dest, size, true)) {
        return false;
    }
    user_access_begin();
    memcpy((void *)user_dest, src, size);
    user_access_end();
    return true;
}
