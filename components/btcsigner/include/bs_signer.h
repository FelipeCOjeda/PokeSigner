// PokeSigner core: BIP32 keys + PSBT v0 analysis/signing for single-sig
// P2WPKH (BIP84) and P2TR key-path (BIP86) inputs.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define BS_MAX_INPUTS   48
#define BS_MAX_OUTPUTS  48
#define BS_MAX_PATH     10

// Platform must provide cryptographically secure randomness.
void bs_platform_random(uint8_t *buf, size_t len);

typedef enum {
    BS_OK = 0,
    BS_ERR_PARSE = -1,
    BS_ERR_UNSUPPORTED = -2,
    BS_ERR_NOTHING_TO_SIGN = -3,
    BS_ERR_MISMATCH = -4,
    BS_ERR_MISSING_UTXO = -5,
    BS_ERR_BUFFER = -6,
    BS_ERR_CRYPTO = -7,
    BS_ERR_NETWORK = -8,
} bs_err_t;

const char *bs_strerror(int err);

// Loads the BIP39 seed (64 bytes). Must be called once before anything else.
int bs_signer_init(const uint8_t seed[64]);
void bs_signer_wipe(void);
uint32_t bs_signer_fingerprint(void);

// JSON: {"fingerprint":"..","network":"..","wpkh":"<descriptor>","tr":"<descriptor>"}
int bs_signer_get_info(int testnet, uint32_t account, char *out, size_t outlen);

typedef struct {
    // raw map spans in the original buffer (key-value pairs, without 0x00 terminator)
    size_t map_start, map_end;
    // from global unsigned tx
    const uint8_t *prevout;   // 36 bytes (txid LE + vout)
    uint32_t sequence;
    // utxo
    int has_utxo;
    uint64_t amount;
    const uint8_t *spk; size_t spk_len;
    int sighash;              // -1 if absent
    // what we will sign
    int sign_type;            // 0 none, 1 p2wpkh, 2 p2tr keypath
    uint32_t path[BS_MAX_PATH]; size_t depth;
    const uint8_t *merkle_root;  // p2tr, may be NULL
} bs_input_t;

typedef struct {
    size_t map_start, map_end;
    uint64_t amount;
    const uint8_t *spk; size_t spk_len;
    int is_change;
} bs_output_t;

typedef struct {
    const uint8_t *buf; size_t len;
    size_t global_end;          // offset of global map terminator
    const uint8_t *tx; size_t tx_len;
    uint32_t tx_version, locktime;
    size_t n_in, n_out;
    bs_input_t in[BS_MAX_INPUTS];
    bs_output_t out[BS_MAX_OUTPUTS];
    int testnet;
    size_t n_sign;
    uint64_t in_total, out_total, change_total, fee;
    int fee_known;
} bs_psbt_t;

// Parses and validates the PSBT, verifies every input/output we claim to own.
int bs_psbt_analyze(const uint8_t *buf, size_t len, bs_psbt_t *p);

// Human-readable summary for the confirmation screen.
int bs_psbt_summary(const bs_psbt_t *p, char *out, size_t outlen);

// Signs every input that belongs to us and writes the PSBT with the new
// partial signatures (PSBT_IN_PARTIAL_SIG / PSBT_IN_TAP_KEY_SIG) appended.
int bs_psbt_sign(const bs_psbt_t *p, uint8_t *out, size_t outcap, size_t *outlen);
