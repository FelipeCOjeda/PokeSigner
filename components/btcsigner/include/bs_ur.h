#pragma once
// Uniform Resources (BCR-2020-005) encoder for "crypto-psbt", used to show a signed
// PSBT as an (animated) QR code that BlueWallet, Sparrow, Nunchuk, etc. can scan.
// Multipart output uses only the "pure" fragments (seqNum <= seqLen), cycled.
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const uint8_t *psbt;  // message = CBOR byte string header + psbt (not copied)
    size_t psbt_len;
    uint8_t hdr[5];
    size_t hdr_len, msg_len, frag_len;
    uint32_t seq_len, crc;  // crc32 of the whole message
    const char *type;       // UPPERCASE UR type, e.g. "CRYPTO-PSBT"
} bs_ur_t;

uint32_t bs_crc32(uint32_t crc, const uint8_t *d, size_t n);   // start with 0
// Splits into fragments of at most max_frag bytes (equal sizes, last one zero-padded).
void bs_ur_init(bs_ur_t *u, const uint8_t *psbt, size_t len, size_t max_frag);
// Any UR type whose CBOR message is already built (e.g. "CRYPTO-ACCOUNT").
void bs_ur_init_raw(bs_ur_t *u, const char *type, const uint8_t *cbor, size_t len, size_t max_frag);
// Writes part `seq` (1-based; wraps around seq_len) as UPPERCASE text, e.g.
// "UR:CRYPTO-PSBT/3-12/LPAX...". With seq_len == 1 writes the single-part form
// "UR:CRYPTO-PSBT/...". Returns the length, or 0 if cap is too small.
size_t bs_ur_part(const bs_ur_t *u, uint32_t seq, char *out, size_t cap);
