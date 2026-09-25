#ifndef VEXA_FUTEX_H
#define VEXA_FUTEX_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Waiting on an address: the core primitive threads synchronize with. A
 * thread waits until another wakes it at the same address, but only starts
 * waiting if the 32-bit value there is still what it expected, so a wake-up
 * between checking and sleeping can't be missed. Native programs use it
 * through vx_wait_address/vx_wake_address; the Linux subsystem builds futex
 * on it. Addresses are per address space (the threads of one process).
 */

struct address_space;

#define FUTEX_ANY 0xffffffffu /* Bitset matching every waiter. */
#define FUTEX_FOREVER (-1)

/* Waits at `address` in the calling process if it holds `expected`. Returns 0
 * once woken, -VX_EAGAIN if the value differed, -VX_ETIMEDOUT, -VX_EINTR, or
 * -VX_EFAULT/-VX_EINVAL for a bad address. */
int futex_wait(uint64_t address, uint32_t expected, uint32_t bitset, int64_t timeout_ms);
/* Wakes up to `count` waiters at `address` whose bitset overlaps `bitset`.
 * Returns how many woke. */
int futex_wake(struct address_space *as, uint64_t address, int count, uint32_t bitset);
/* Wakes up to `wake` waiters at `address` and moves up to `move` more to wait
 * at `target` instead. With `compare`, first checks the value at `address`
 * is `expected` (-VX_EAGAIN if not). Returns how many woke (plus moved). */
int futex_requeue(uint64_t address, int wake, uint64_t target, int move, bool compare,
                  uint32_t expected);

#endif
