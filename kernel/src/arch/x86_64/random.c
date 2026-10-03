/* The kernel's random numbers: a cryptographic generator, as Linux's.
 *
 * Entropy (the CPU's RDSEED and RDRAND where it has them, the cycle counter,
 * and the timing of every interrupt) is hashed into a pool with BLAKE2s. The
 * output comes from ChaCha20 keyed by the pool's hash: each request makes
 * fresh key material first and throws the old key away ("fast key erasure"),
 * so what was given out before can't be worked out from the state now. The
 * key is remade from the pool every few seconds while new entropy arrives.
 */
#include <stdbool.h>
#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/random.h>
#include <vexa/spinlock.h>
#include <vexa/string.h>

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

static bool rdseed(uint64_t *value) {
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    if (a < 7) {
        return false;
    }
    cpuid(7, &a, &b, &c, &d);
    if (!(b & (1U << 18))) {
        return false;
    }
    for (int tries = 0; tries < 10; tries++) {
        uint8_t ok;
        __asm__ volatile("rdseed %0; setc %1" : "=r"(*value), "=qm"(ok));
        if (ok) {
            return true;
        }
    }
    return false;
}

/* ---- BLAKE2s (RFC 7693), unkeyed, 32-byte digests ---- */

struct blake2s {
    uint32_t h[8], t[2];
    uint8_t buffer[64];
    size_t used;
};

static const uint32_t blake2s_iv[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                       0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};

static const uint8_t blake2s_sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

static uint32_t ror32(uint32_t x, int n) {
    return x >> n | x << (32 - n);
}

static uint32_t load32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void store32(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)x, p[1] = (uint8_t)(x >> 8), p[2] = (uint8_t)(x >> 16), p[3] = (uint8_t)(x >> 24);
}

static void blake2s_compress(struct blake2s *s, const uint8_t block[64], bool last) {
    uint32_t m[16], v[16];
    for (int i = 0; i < 16; i++) {
        m[i] = load32(block + 4 * i);
    }
    for (int i = 0; i < 8; i++) {
        v[i] = s->h[i];
        v[i + 8] = blake2s_iv[i];
    }
    v[12] ^= s->t[0];
    v[13] ^= s->t[1];
    if (last) {
        v[14] = ~v[14];
    }
#define G(a, b, c, d, x, y)                                                                        \
    do {                                                                                           \
        v[a] = v[a] + v[b] + (x), v[d] = ror32(v[d] ^ v[a], 16);                                   \
        v[c] = v[c] + v[d], v[b] = ror32(v[b] ^ v[c], 12);                                         \
        v[a] = v[a] + v[b] + (y), v[d] = ror32(v[d] ^ v[a], 8);                                    \
        v[c] = v[c] + v[d], v[b] = ror32(v[b] ^ v[c], 7);                                          \
    } while (0)
    for (int r = 0; r < 10; r++) {
        const uint8_t *s_ = blake2s_sigma[r];
        G(0, 4, 8, 12, m[s_[0]], m[s_[1]]);
        G(1, 5, 9, 13, m[s_[2]], m[s_[3]]);
        G(2, 6, 10, 14, m[s_[4]], m[s_[5]]);
        G(3, 7, 11, 15, m[s_[6]], m[s_[7]]);
        G(0, 5, 10, 15, m[s_[8]], m[s_[9]]);
        G(1, 6, 11, 12, m[s_[10]], m[s_[11]]);
        G(2, 7, 8, 13, m[s_[12]], m[s_[13]]);
        G(3, 4, 9, 14, m[s_[14]], m[s_[15]]);
    }
#undef G
    for (int i = 0; i < 8; i++) {
        s->h[i] ^= v[i] ^ v[i + 8];
    }
}

static void blake2s_init(struct blake2s *s) {
    memset(s, 0, sizeof(*s));
    memcpy(s->h, blake2s_iv, sizeof(s->h));
    s->h[0] ^= 0x01010000 | 32; /* No key, a 32-byte digest. */
}

static void blake2s_update(struct blake2s *s, const void *data, size_t size) {
    const uint8_t *in = data;
    while (size) {
        if (s->used == 64) { /* A full block, and more coming: it isn't the last. */
            s->t[0] += 64;
            s->t[1] += s->t[0] < 64;
            blake2s_compress(s, s->buffer, false);
            s->used = 0;
        }
        size_t n = 64 - s->used < size ? 64 - s->used : size;
        memcpy(s->buffer + s->used, in, n);
        s->used += n, in += n, size -= n;
    }
}

static void blake2s_final(struct blake2s *s, uint8_t out[32]) {
    s->t[0] += (uint32_t)s->used;
    s->t[1] += s->t[0] < s->used;
    memset(s->buffer + s->used, 0, 64 - s->used);
    blake2s_compress(s, s->buffer, true);
    for (int i = 0; i < 8; i++) {
        store32(out + 4 * i, s->h[i]);
    }
}

/* ---- ChaCha20 (RFC 8439) ---- */

static void chacha20_block(const uint8_t key[32], uint64_t counter, uint8_t out[64]) {
    uint32_t x[16], in[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) {
        in[4 + i] = load32(key + 4 * i);
    }
    in[12] = (uint32_t)counter;
    in[13] = (uint32_t)(counter >> 32);
    in[14] = in[15] = 0;
    memcpy(x, in, sizeof(x));
#define QR(a, b, c, d)                                                                             \
    do {                                                                                           \
        x[a] += x[b], x[d] = ror32(x[d] ^ x[a], 16);                                               \
        x[c] += x[d], x[b] = ror32(x[b] ^ x[c], 20);                                               \
        x[a] += x[b], x[d] = ror32(x[d] ^ x[a], 24);                                               \
        x[c] += x[d], x[b] = ror32(x[b] ^ x[c], 25);                                               \
    } while (0)
    for (int i = 0; i < 10; i++) {
        QR(0, 4, 8, 12);
        QR(1, 5, 9, 13);
        QR(2, 6, 10, 14);
        QR(3, 7, 11, 15);
        QR(0, 5, 10, 15);
        QR(1, 6, 11, 12);
        QR(2, 7, 8, 13);
        QR(3, 4, 9, 14);
    }
#undef QR
    for (int i = 0; i < 16; i++) {
        store32(out + 4 * i, x[i] + in[i]);
    }
}

/* ---- The generator ---- */

static struct spinlock lock = SPINLOCK_INIT;
static struct blake2s pool;           /* Entropy since the key was last made. */
static bool pool_fresh;               /* Something went into it since then. */
static uint8_t key[32];               /* ChaCha20's key. */
static bool seeded;
static uint64_t reseeded_ms;
#define RESEED_MS 5000

/* Interrupt timings, gathered cheaply and hashed into the pool now and then. */
static uint64_t fast_pool[4];
static unsigned fast_count;

static void add_cpu_entropy(struct blake2s *s) {
    uint64_t words[16];
    for (int i = 0; i < 16; i++) {
        uint64_t hw = 0;
        if (!rdseed(&hw)) {
            rdrand(&hw);
        }
        words[i] = hw ^ rdtsc();
    }
    blake2s_update(s, words, sizeof(words));
}

/* With the lock held: a new key from the old one and the pool. */
static void reseed(void) {
    if (!seeded) {
        blake2s_init(&pool);
    }
    add_cpu_entropy(&pool);
    uint64_t now[2] = {rdtsc(), timer_ms()};
    blake2s_update(&pool, now, sizeof(now));
    blake2s_update(&pool, fast_pool, sizeof(fast_pool));
    blake2s_update(&pool, key, sizeof(key));
    blake2s_final(&pool, key);
    blake2s_init(&pool);
    pool_fresh = false;
    seeded = true;
    reseeded_ms = timer_ms();
}

void random_add(const void *data, size_t size) {
    uint64_t flags = spin_lock_irqsave(&lock);
    if (!seeded) {
        reseed();
    }
    blake2s_update(&pool, data, size);
    pool_fresh = true;
    spin_unlock_irqrestore(&lock, flags);
}

void random_interrupt(unsigned vector) {
    /* A few cycles per interrupt: the low bits of the cycle counter are what's
     * unpredictable. Every 64 interrupts, the lot goes into the pool. */
    uint64_t t = rdtsc() ^ ((uint64_t)vector << 56);
    unsigned n = __atomic_fetch_add(&fast_count, 1, __ATOMIC_RELAXED);
    uint64_t *slot = &fast_pool[n & 3];
    *slot = (*slot << 7 | *slot >> 57) ^ t;
    if ((n & 63) == 63 && seeded) {
        uint64_t copy[4];
        memcpy(copy, fast_pool, sizeof(copy));
        random_add(copy, sizeof(copy));
    }
}

void random_bytes(void *buffer, size_t size) {
    uint8_t *out = buffer;
    while (size) {
        /* At most a page at a time with interrupts off. */
        size_t n = size < 4096 ? size : 4096;
        uint64_t flags = spin_lock_irqsave(&lock);
        if (!seeded || (pool_fresh && timer_ms() - reseeded_ms >= RESEED_MS)) {
            reseed();
        }
        uint8_t block[64];
        uint64_t counter = 1;
        for (size_t done = 0; done < n; done += 64) {
            chacha20_block(key, counter++, block);
            memcpy(out + done, block, n - done < 64 ? n - done : 64);
        }
        chacha20_block(key, 0, block); /* The next key: this output can't be redone. */
        memcpy(key, block, 32);
        memset(block, 0, sizeof(block));
        spin_unlock_irqrestore(&lock, flags);
        out += n, size -= n;
    }
}
