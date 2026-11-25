#pragma once
#include <stdint.h>
#include <stddef.h>

int bs_base58check(const uint8_t *data, size_t len, char *out, size_t outlen);
int bs_segwit_addr(const char *hrp, int witver, const uint8_t *prog, size_t plen, char *out, size_t outlen);
// Returns -1 for scripts with no address form (OP_RETURN, bare multisig...).
int bs_script_to_addr(const uint8_t *spk, size_t n, int testnet, char *out, size_t outlen);
