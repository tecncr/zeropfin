/* sha256.h — small FIPS 180-4 SHA-256 (used to store local session tokens hashed). */
#ifndef ZF_SHA256_H
#define ZF_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t block[64];
    size_t used;
} zf_sha256;

void zf_sha256_init(zf_sha256 *c);
void zf_sha256_update(zf_sha256 *c, const void *data, size_t len);
void zf_sha256_final(zf_sha256 *c, uint8_t out[32]);
void zf_sha256_hex(const void *data, size_t len, char out[65]);

#endif
