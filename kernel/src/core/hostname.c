#include <vexa/abi.h>
#include <vexa/hostname.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>

static char hostname[HOSTNAME_MAX + 1] = "vexa";
static struct spinlock lock = SPINLOCK_INIT;

void hostname_get(char out[HOSTNAME_MAX + 1]) {
    uint64_t flags = spin_lock_irqsave(&lock);
    memcpy(out, hostname, sizeof(hostname));
    spin_unlock_irqrestore(&lock, flags);
}

int hostname_set(const char *name, size_t length) {
    if (length == 0 || length > HOSTNAME_MAX) {
        return -VX_EINVAL;
    }
    for (size_t i = 0; i < length; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.')) {
            return -VX_EINVAL;
        }
    }
    uint64_t flags = spin_lock_irqsave(&lock);
    memcpy(hostname, name, length);
    hostname[length] = '\0';
    spin_unlock_irqrestore(&lock, flags);
    return 0;
}
