#ifndef VEXA_HASH_H
#define VEXA_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SHA-256, a piece at a time:
 *
 *     struct vx_sha256 h;
 *     vx_sha256_init(&h);
 *     vx_sha256_add(&h, data, size);   (as often as needed)
 *     vx_sha256_end(&h, digest);       (32 bytes) */
struct vx_sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
};

void vx_sha256_init(struct vx_sha256 *h);
void vx_sha256_add(struct vx_sha256 *h, const void *data, size_t size);
void vx_sha256_end(struct vx_sha256 *h, uint8_t digest[32]);

#ifdef __cplusplus
}
#endif

#endif
