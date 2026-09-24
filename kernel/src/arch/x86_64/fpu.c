#include <stdbool.h>
#include <stdint.h>
#include <vexa/arch.h>
#include <vexa/fpu.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>

#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR0_TS (1ULL << 3)
#define CR0_NE (1ULL << 5)
#define CR4_OSFXSR (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_OSXSAVE (1ULL << 18)

#define XCR0_X87 (1ULL << 0)
#define XCR0_SSE (1ULL << 1)
#define XCR0_AVX (1ULL << 2)
#define XCR0_AVX512 (7ULL << 5) /* Opmask, ZMM_Hi256, Hi16_ZMM. */

static bool initialized; /* Set once the bootstrap CPU has chosen the settings. */
static bool use_xsave;
static uint64_t xcr0;
static size_t state_size = 512; /* FXSAVE's fixed size. */
/* The state a new thread starts with, captured right after FNINIT. */
static uint8_t initial_state[PAGE_SIZE] __attribute__((aligned(64)));

static uint64_t read_cr0(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static uint64_t read_cr4(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr4, %0" : "=r"(v));
    return v;
}

static void xsetbv(uint32_t reg, uint64_t value) {
    __asm__ volatile("xsetbv" : : "c"(reg), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

void fpu_init_cpu(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    bool has_xsave = c & (1U << 26);
    bool has_avx = c & (1U << 28);

    uint64_t cr0 = (read_cr0() & ~(CR0_EM | CR0_TS)) | CR0_MP | CR0_NE;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
    uint64_t cr4 = read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT;
    if (has_xsave) {
        cr4 |= CR4_OSXSAVE;
    }
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    if (has_xsave) {
        uint64_t want = XCR0_X87 | XCR0_SSE;
        if (has_avx) {
            want |= XCR0_AVX;
        }
        uint32_t lo, hi;
        /* Leaf 0xd, subleaf 0: which state components the CPU supports. */
        __asm__ volatile("cpuid" : "=a"(lo), "=b"(b), "=c"(c), "=d"(hi) : "a"(0xd), "c"(0));
        uint64_t supported = (uint64_t)hi << 32 | lo;
        if ((supported & XCR0_AVX512) == XCR0_AVX512 && (want & XCR0_AVX)) {
            want |= XCR0_AVX512;
        }
        want &= supported;
        if (initialized) {
            want = xcr0; /* Every CPU must use the same layout. */
        }
        xsetbv(0, want);
        if (!initialized) {
            xcr0 = want;
            use_xsave = true;
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0xd), "c"(0));
            state_size = b; /* Size needed for the components enabled in XCR0. */
        }
    }

    __asm__ volatile("fninit");
    if (!initialized) {
        if (state_size > sizeof(initial_state)) {
            panic("fpu: %lu-byte register state is too large", state_size);
        }
        fpu_save(initial_state);
        initialized = true;
    }
}

void *fpu_alloc_state(void) {
    uint64_t phys = pmm_alloc(0); /* The state always fits in a page (checked above). */
    if (!phys) {
        return NULL;
    }
    void *state = phys_to_virt(phys);
    memcpy(state, initial_state, state_size);
    return state;
}

void fpu_free_state(void *state) {
    if (state) {
        pmm_free(virt_to_phys(state), 0);
    }
}

void fpu_save(void *state) {
    if (use_xsave) {
        __asm__ volatile("xsave64 (%0)" : : "r"(state), "a"(0xffffffff), "d"(0xffffffff)
                         : "memory");
    } else {
        __asm__ volatile("fxsave64 (%0)" : : "r"(state) : "memory");
    }
}

void fpu_restore(void *state) {
    if (use_xsave) {
        __asm__ volatile("xrstor64 (%0)" : : "r"(state), "a"(0xffffffff), "d"(0xffffffff)
                         : "memory");
    } else {
        __asm__ volatile("fxrstor64 (%0)" : : "r"(state) : "memory");
    }
}

const char *fpu_describe(void) {
    if (!use_xsave) {
        return "SSE via FXSAVE (512 bytes)";
    }
    if ((xcr0 & XCR0_AVX512) == XCR0_AVX512) {
        return "XSAVE with AVX-512";
    }
    if (xcr0 & XCR0_AVX) {
        return "XSAVE with AVX";
    }
    return "XSAVE with SSE";
}
