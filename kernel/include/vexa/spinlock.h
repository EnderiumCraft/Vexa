#ifndef VEXA_SPINLOCK_H
#define VEXA_SPINLOCK_H

#include <stdint.h>

/* A spinlock that also disables interrupts on this CPU while held, so an
 * interrupt handler can never deadlock against the code it interrupted.
 *
 *     uint64_t flags = spin_lock_irqsave(&lock);
 *     ...
 *     spin_unlock_irqrestore(&lock, flags);
 */
struct spinlock {
    volatile uint32_t locked;
};

/* While a CPU spins, with interrupts off, it still answers TLB flush requests
 * from other CPUs (see tlb.c), so a CPU that holds a lock and waits for the
 * flush can't deadlock with one waiting for that lock. */
extern volatile uint32_t tlb_requests_possible;
void tlb_service(void);

#define SPINLOCK_INIT {0}
#define RFLAGS_IF (1ULL << 9)

static inline uint64_t spin_lock_irqsave(struct spinlock *lock) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) {
            if (tlb_requests_possible) {
                tlb_service();
            }
            __asm__ volatile("pause");
        }
    }
    return flags;
}

static inline void spin_unlock(struct spinlock *lock) {
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
}

static inline void spin_unlock_irqrestore(struct spinlock *lock, uint64_t flags) {
    spin_unlock(lock);
    if (flags & RFLAGS_IF) {
        __asm__ volatile("sti" : : : "memory");
    }
}

#endif
