#include "bs_bip39.h"
#include "bs_crypto.h"
#include <string.h>
#include <ctype.h>

int bs_bip39_word_index(const char *word, size_t len) {
    int lo = 0, hi = 2047;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *w = BS_BIP39_WORDS[mid];
        size_t wl = strlen(w);
        int c = strncmp(word, w, len < wl ? len : wl);
        if (c == 0) c = (len < wl) ? -1 : (len > wl ? 1 : 0);
        if (c == 0) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

int bs_bip39_from_entropy(const uint8_t *entropy, size_t len, char *out, size_t outlen) {
    if (len < 16 || len > 32 || len % 4) return -1;
    uint8_t buf[33], h[32];
    memcpy(buf, entropy, len);
    bs_sha256(entropy, len, h);
    buf[len] = h[0];
    size_t nwords = (len * 8 + len / 4) / 11, pos = 0;
    for (size_t i = 0; i < nwords; i++) {
        uint32_t idx = 0;
        for (size_t b = 0; b < 11; b++) {
            size_t bit = i * 11 + b;
            idx = (idx << 1) | ((buf[bit / 8] >> (7 - bit % 8)) & 1);
        }
        const char *w = BS_BIP39_WORDS[idx];
        size_t wl = strlen(w);
        if (pos + wl + 2 > outlen) { bs_wipe(buf, sizeof(buf)); return -1; }
        if (i) out[pos++] = ' ';
        memcpy(out + pos, w, wl); pos += wl;
    }
    out[pos] = 0;
    bs_wipe(buf, sizeof(buf)); bs_wipe(h, sizeof(h));
    return 0;
}

int bs_bip39_to_entropy(const char *m, uint8_t *entropy, size_t *len) {
    uint16_t idx[24];
    size_t n = 0;
    const char *p = m;
    char word[16];
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        size_t wl = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
            if (wl >= sizeof(word)) return -1;
            word[wl++] = (char)tolower((unsigned char)*p++);
        }
        if (n >= 24) return -1;
        int i = bs_bip39_word_index(word, wl);
        if (i < 0) return -2;
        idx[n++] = (uint16_t)i;
    }
    if (n < 12 || n % 3) return -1;
    size_t bits = n * 11, ent = bits * 32 / 33 / 8, csbits = bits - ent * 8;
    uint8_t buf[33] = {0}, h[32];
    for (size_t i = 0; i < n; i++)
        for (size_t b = 0; b < 11; b++)
            if (idx[i] & (1 << (10 - b))) { size_t bit = i * 11 + b; buf[bit / 8] |= 1 << (7 - bit % 8); }
    bs_sha256(buf, ent, h);
    uint8_t mask = (uint8_t)(0xff << (8 - csbits));
    int ok = (buf[ent] & mask) == (h[0] & mask);
    if (ok) { memcpy(entropy, buf, ent); *len = ent; }
    bs_wipe(buf, sizeof(buf)); bs_wipe(idx, sizeof(idx));
    return ok ? 0 : -3;
}

int bs_bip39_normalize(const char *m, char *out, size_t outlen) {
    uint8_t ent[32]; size_t len;
    int r = bs_bip39_to_entropy(m, ent, &len);
    if (r) return r;
    r = bs_bip39_from_entropy(ent, len, out, outlen);
    bs_wipe(ent, sizeof(ent));
    return r;
}

void bs_bip39_seed(const char *mnemonic, const char *passphrase, uint8_t seed[64]) {
    char salt[8 + 256];
    size_t pl = passphrase ? strlen(passphrase) : 0;
    if (pl > 256) pl = 256;
    memcpy(salt, "mnemonic", 8);
    if (pl) memcpy(salt + 8, passphrase, pl);
    bs_pbkdf2_hmac_sha512((const uint8_t *)mnemonic, strlen(mnemonic),
                          (const uint8_t *)salt, 8 + pl, 2048, seed);
    bs_wipe(salt, sizeof(salt));
}
