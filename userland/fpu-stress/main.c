/*
 * Checks that the kernel saves and restores vector registers correctly.
 *
 * Fills the 16 vector registers (256-bit YMM with AVX, else 128-bit XMM) with
 * values unique to this process, spins long enough to be preempted many times
 * (and, on a multi-core machine, possibly moved to another CPU), then checks
 * every register still holds its value. Run several copies at once:
 *
 *     spawn fpu-stress
 *     spawn fpu-stress
 *     run fpu-stress
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vexa/syscall.h>

#define ROUNDS 20
#define SPIN_ITERATIONS 40000000

static bool avx_usable(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    bool avx = c & (1U << 28), osxsave = c & (1U << 27);
    if (!avx || !osxsave) {
        return false;
    }
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6; /* The kernel saves both SSE and AVX state. */
}

/* Loads all 16 registers from `in`, spins, then stores them to `out`. One asm
 * block, so the compiler can't use the registers in between. */
static void run_sse(const uint8_t *in, uint8_t *out) {
    __asm__ volatile(
        "movdqu 0(%0), %%xmm0\n movdqu 16(%0), %%xmm1\n movdqu 32(%0), %%xmm2\n"
        "movdqu 48(%0), %%xmm3\n movdqu 64(%0), %%xmm4\n movdqu 80(%0), %%xmm5\n"
        "movdqu 96(%0), %%xmm6\n movdqu 112(%0), %%xmm7\n movdqu 128(%0), %%xmm8\n"
        "movdqu 144(%0), %%xmm9\n movdqu 160(%0), %%xmm10\n movdqu 176(%0), %%xmm11\n"
        "movdqu 192(%0), %%xmm12\n movdqu 208(%0), %%xmm13\n movdqu 224(%0), %%xmm14\n"
        "movdqu 240(%0), %%xmm15\n"
        "1: dec %2\n jnz 1b\n"
        "movdqu %%xmm0, 0(%1)\n movdqu %%xmm1, 16(%1)\n movdqu %%xmm2, 32(%1)\n"
        "movdqu %%xmm3, 48(%1)\n movdqu %%xmm4, 64(%1)\n movdqu %%xmm5, 80(%1)\n"
        "movdqu %%xmm6, 96(%1)\n movdqu %%xmm7, 112(%1)\n movdqu %%xmm8, 128(%1)\n"
        "movdqu %%xmm9, 144(%1)\n movdqu %%xmm10, 160(%1)\n movdqu %%xmm11, 176(%1)\n"
        "movdqu %%xmm12, 192(%1)\n movdqu %%xmm13, 208(%1)\n movdqu %%xmm14, 224(%1)\n"
        "movdqu %%xmm15, 240(%1)\n"
        : : "r"(in), "r"(out), "r"((uint64_t)SPIN_ITERATIONS)
        : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
          "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

static void run_avx(const uint8_t *in, uint8_t *out) {
    __asm__ volatile(
        "vmovdqu 0(%0), %%ymm0\n vmovdqu 32(%0), %%ymm1\n vmovdqu 64(%0), %%ymm2\n"
        "vmovdqu 96(%0), %%ymm3\n vmovdqu 128(%0), %%ymm4\n vmovdqu 160(%0), %%ymm5\n"
        "vmovdqu 192(%0), %%ymm6\n vmovdqu 224(%0), %%ymm7\n vmovdqu 256(%0), %%ymm8\n"
        "vmovdqu 288(%0), %%ymm9\n vmovdqu 320(%0), %%ymm10\n vmovdqu 352(%0), %%ymm11\n"
        "vmovdqu 384(%0), %%ymm12\n vmovdqu 416(%0), %%ymm13\n vmovdqu 448(%0), %%ymm14\n"
        "vmovdqu 480(%0), %%ymm15\n"
        "1: dec %2\n jnz 1b\n"
        "vmovdqu %%ymm0, 0(%1)\n vmovdqu %%ymm1, 32(%1)\n vmovdqu %%ymm2, 64(%1)\n"
        "vmovdqu %%ymm3, 96(%1)\n vmovdqu %%ymm4, 128(%1)\n vmovdqu %%ymm5, 160(%1)\n"
        "vmovdqu %%ymm6, 192(%1)\n vmovdqu %%ymm7, 224(%1)\n vmovdqu %%ymm8, 256(%1)\n"
        "vmovdqu %%ymm9, 288(%1)\n vmovdqu %%ymm10, 320(%1)\n vmovdqu %%ymm11, 352(%1)\n"
        "vmovdqu %%ymm12, 384(%1)\n vmovdqu %%ymm13, 416(%1)\n vmovdqu %%ymm14, 448(%1)\n"
        "vmovdqu %%ymm15, 480(%1)\n"
        "vzeroupper\n"
        : : "r"(in), "r"(out), "r"((uint64_t)SPIN_ITERATIONS)
        : "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8",
          "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

int main(void) {
    bool avx = avx_usable();
    size_t size = avx ? 16 * 32 : 16 * 16;
    long id = vx_process_id();
    uint8_t in[16 * 32], out[16 * 32];
    long start = vx_uptime();

    for (int round = 0; round < ROUNDS; round++) {
        for (size_t i = 0; i < size; i++) {
            in[i] = (uint8_t)(id * 37 + round * 11 + i * 7);
        }
        memset(out, 0, sizeof(out));
        if (avx) {
            run_avx(in, out);
        } else {
            run_sse(in, out);
        }
        if (memcmp(in, out, size) != 0) {
            printf("fpu-stress (process %ld): FAILED in round %d: registers changed\n", id, round);
            return 1;
        }
    }
    printf("fpu-stress (process %ld): passed, %d rounds with %s registers in %ld ms\n", id,
           ROUNDS, avx ? "AVX" : "SSE", vx_uptime() - start);
    return 0;
}
