// Portable hash primitives (no external deps) used by the signer core.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct { uint32_t s[8]; uint8_t buf[64]; uint64_t len; } bs_sha256_ctx;
typedef struct { uint64_t s[8]; uint8_t buf[128]; uint64_t len; } bs_sha512_ctx;

void bs_sha256_init(bs_sha256_ctx *c);
void bs_sha256_update(bs_sha256_ctx *c, const void *data, size_t len);
void bs_sha256_final(bs_sha256_ctx *c, uint8_t out[32]);
void bs_sha256(const void *data, size_t len, uint8_t out[32]);
void bs_sha256d(const void *data, size_t len, uint8_t out[32]);

void bs_sha512_init(bs_sha512_ctx *c);
void bs_sha512_update(bs_sha512_ctx *c, const void *data, size_t len);
void bs_sha512_final(bs_sha512_ctx *c, uint8_t out[64]);

void bs_ripemd160(const void *data, size_t len, uint8_t out[20]);
void bs_hash160(const void *data, size_t len, uint8_t out[20]);

void bs_hmac_sha512(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[64]);
void bs_pbkdf2_hmac_sha512(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen,
                           uint32_t iters, uint8_t out[64]);

// BIP340 tagged hash: SHA256(SHA256(tag)||SHA256(tag)||msg). Use init+update+final.
void bs_tagged_init(bs_sha256_ctx *c, const char *tag);

// Best-effort secure wipe.
void bs_wipe(void *p, size_t n);
