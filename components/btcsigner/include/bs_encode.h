#pragma once
#include <stdint.h>
#include <stddef.h>

// Base64 (RFC 4648). decode returns the length or -1; works in place (out == in).
int bs_base64_decode(const char *in, size_t n, uint8_t *out, size_t cap);
size_t bs_base64_encode(const uint8_t *in, size_t n, char *out, size_t cap);   // 0 = no room
int bs_base58check(const uint8_t *data, size_t len, char *out, size_t outlen);
int bs_segwit_addr(const char *hrp, int witver, const uint8_t *prog, size_t plen, char *out, size_t outlen);
// Returns -1 for scripts with no address form (OP_RETURN, bare multisig...).
int bs_script_to_addr(const uint8_t *spk, size_t n, int testnet, char *out, size_t outlen);
