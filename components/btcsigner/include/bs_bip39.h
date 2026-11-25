#pragma once
#include <stdint.h>
#include <stddef.h>

extern const char *const BS_BIP39_WORDS[2048];

// entropy: 16..32 bytes (multiple of 4). Writes space-separated words.
// Returns 0 on success.
int bs_bip39_from_entropy(const uint8_t *entropy, size_t len, char *out, size_t outlen);

// Parses and validates a mnemonic (12/15/18/21/24 words, lowercase, single
// or multiple spaces). On success writes the entropy and its length.
int bs_bip39_to_entropy(const char *mnemonic, uint8_t *entropy, size_t *len);

// Normalizes (single spaces, lowercase) a valid mnemonic into out.
int bs_bip39_normalize(const char *mnemonic, char *out, size_t outlen);

// BIP39 seed (PBKDF2-HMAC-SHA512, 2048 rounds). ASCII mnemonic/passphrase only.
void bs_bip39_seed(const char *mnemonic, const char *passphrase, uint8_t seed[64]);

int bs_bip39_word_index(const char *word, size_t len);
