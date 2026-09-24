#include <stdbool.h>
#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/random.h>

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static bool rdrand(uint64_t *value) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(c & (1U << 30))) {
        return false;
    }
    for (int tries = 0; tries < 10; tries++) {
        uint8_t ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*value), "=qm"(ok));
        if (ok) {
            return true;
        }
    }
    return false;
}

void random_bytes(void *buffer, size_t size) {
    static uint64_t state = 0x9e3779b97f4a7c15ULL;
    uint8_t *out = buffer;
    for (size_t i = 0; i < size; i += 8) {
        uint64_t hw = 0;
        rdrand(&hw);
        /* splitmix64 over the counter, the hardware value and the running state. */
        uint64_t z = (state += 0x9e3779b97f4a7c15ULL) ^ rdtsc() ^ hw;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z ^= z >> 31;
        for (size_t j = 0; j < 8 && i + j < size; j++) {
            out[i + j] = (uint8_t)(z >> (8 * j));
        }
    }
}
