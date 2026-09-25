#include <vexa/abi.h>
#include <vexa/futex.h>
#include <vexa/mm.h>
#include <vexa/process.h>
#include <vexa/sched.h>
#include <vexa/spinlock.h>
#include <vexa/uaccess.h>

/* Every waiter is on one list, under one lock: simple, and fine until there
 * are workloads with many contended locks to measure. Each waiter has its own
 * wait queue, so a wake-up goes to exactly the threads chosen. Waiters live on
 * their thread's stack; they're only unlinked under futex_lock, and a waiter
 * doesn't return until it has seen that. Lock order: futex_lock, then the
 * scheduler lock (inside wait_queue_wake_all), or an address space's lock
 * (reading the value). */

/* What a waiter waits on: an address in an address space, or, in memory
 * shared between processes, a physical address (as == NULL), so waits and
 * wakes from different processes meet, as with Linux's shared futexes. */
struct futex_key {
    struct address_space *as;
    uint64_t address;
};

static struct futex_key key_for(struct address_space *as, uint64_t address) {
    uint64_t phys;
    if (vm_shared_physical(as, address, &phys)) {
        return (struct futex_key){NULL, phys};
    }
    return (struct futex_key){as, address};
}

struct futex_waiter {
    struct address_space *as;
    uint64_t address;
    uint32_t bitset;
    bool woken;
    struct wait_queue queue;
    struct futex_waiter *next;
};

static struct spinlock futex_lock = SPINLOCK_INIT;
static struct futex_waiter *waiters;

static bool woken(void *arg) {
    return ((struct futex_waiter *)arg)->woken;
}

static void unlink_waiter(struct futex_waiter *waiter) {
    for (struct futex_waiter **link = &waiters; *link; link = &(*link)->next) {
        if (*link == waiter) {
            *link = waiter->next;
            return;
        }
    }
}

static bool valid(uint64_t address) {
    return address % 4 == 0 && address >= USER_BASE && address + 4 <= USER_END;
}

int futex_wait(uint64_t address, uint32_t expected, uint32_t bitset, int64_t timeout_ms) {
    if (!valid(address) || bitset == 0) {
        return -VX_EINVAL;
    }
    struct futex_key key = key_for(process_current()->address_space, address);
    struct futex_waiter waiter = {.as = key.as, .address = key.address, .bitset = bitset};
    uint64_t flags = spin_lock_irqsave(&futex_lock);
    uint32_t value;
    if (!copy_from_user(&value, address, sizeof(value))) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -VX_EFAULT;
    }
    if (value != expected) {
        spin_unlock_irqrestore(&futex_lock, flags);
        return -VX_EAGAIN;
    }
    waiter.next = waiters;
    waiters = &waiter;
    spin_unlock_irqrestore(&futex_lock, flags);

    int result = timeout_ms < 0
                     ? wait_queue_wait_interruptible(&waiter.queue, woken, &waiter)
                     : wait_queue_wait_timeout(&waiter.queue, woken, &waiter, (uint64_t)timeout_ms,
                                               true);
    flags = spin_lock_irqsave(&futex_lock);
    if (waiter.woken) {
        result = 0; /* Woken just as the wait gave up: the wake-up counts. */
    } else {
        unlink_waiter(&waiter);
    }
    spin_unlock_irqrestore(&futex_lock, flags);
    return result;
}

static int wake_locked(struct address_space *as, uint64_t address, int count, uint32_t bitset) {
    int woke = 0;
    for (struct futex_waiter **link = &waiters; *link && woke < count;) {
        struct futex_waiter *waiter = *link;
        if (waiter->as == as && waiter->address == address && (waiter->bitset & bitset)) {
            *link = waiter->next;
            waiter->woken = true;
            wait_queue_wake_all(&waiter->queue);
            woke++;
        } else {
            link = &waiter->next;
        }
    }
    return woke;
}

int futex_wake(struct address_space *as, uint64_t address, int count, uint32_t bitset) {
    if (!valid(address) || count <= 0) {
        return count <= 0 ? 0 : -VX_EINVAL;
    }
    struct futex_key key = key_for(as, address);
    uint64_t flags = spin_lock_irqsave(&futex_lock);
    int woke = wake_locked(key.as, key.address, count, bitset);
    spin_unlock_irqrestore(&futex_lock, flags);
    return woke;
}

int futex_requeue(uint64_t address, int wake, uint64_t target, int move, bool compare,
                  uint32_t expected) {
    if (!valid(address) || !valid(target) || wake < 0 || move < 0) {
        return -VX_EINVAL;
    }
    struct address_space *as = process_current()->address_space;
    struct futex_key from = key_for(as, address), to = key_for(as, target);
    uint64_t flags = spin_lock_irqsave(&futex_lock);
    if (compare) {
        uint32_t value;
        if (!copy_from_user(&value, address, sizeof(value))) {
            spin_unlock_irqrestore(&futex_lock, flags);
            return -VX_EFAULT;
        }
        if (value != expected) {
            spin_unlock_irqrestore(&futex_lock, flags);
            return -VX_EAGAIN;
        }
    }
    int done = wake_locked(from.as, from.address, wake, FUTEX_ANY);
    for (struct futex_waiter *waiter = waiters; waiter && move > 0; waiter = waiter->next) {
        if (waiter->as == from.as && waiter->address == from.address) {
            waiter->as = to.as;
            waiter->address = to.address;
            move--;
            done++;
        }
    }
    spin_unlock_irqrestore(&futex_lock, flags);
    return done;
}
