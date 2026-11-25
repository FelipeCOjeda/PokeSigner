// Base58Check, Bech32/Bech32m (BIP173/BIP350) encoders and scriptPubKey -> address.
#include "bs_encode.h"
#include "bs_crypto.h"
#include <string.h>

static const char B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

int bs_base58check(const uint8_t *data, size_t len, char *out, size_t outlen) {
    uint8_t buf[128], h[32];
    if (len + 4 > sizeof(buf)) return -1;
    memcpy(buf, data, len);
    bs_sha256d(data, len, h);
    memcpy(buf + len, h, 4);
    len += 4;
    size_t zeros = 0;
    while (zeros < len && buf[zeros] == 0) zeros++;
    uint8_t b58[200] = {0};
    size_t size = len * 138 / 100 + 1, length = 0;
    for (size_t i = zeros; i < len; i++) {
        int carry = buf[i];
        size_t k = 0;
        for (size_t j = size; j-- > 0 && (carry || k < length); k++) {
            carry += 256 * b58[j];
            b58[j] = carry % 58;
            carry /= 58;
        }
        length = k;
    }
    size_t j = size - length;
    while (j < size && b58[j] == 0) j++;
    if (zeros + (size - j) + 1 > outlen) return -1;
    size_t o = 0;
    for (size_t i = 0; i < zeros; i++) out[o++] = '1';
    for (; j < size; j++) out[o++] = B58[b58[j]];
    out[o] = 0;
    return 0;
}

static const char B32[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static uint32_t polymod(const uint8_t *v, size_t n) {
    static const uint32_t G[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
    uint32_t chk = 1;
    for (size_t i = 0; i < n; i++) {
        uint8_t top = chk >> 25;
        chk = (chk & 0x1ffffff) << 5 ^ v[i];
        for (int k = 0; k < 5; k++) if ((top >> k) & 1) chk ^= G[k];
    }
    return chk;
}

int bs_segwit_addr(const char *hrp, int witver, const uint8_t *prog, size_t plen, char *out, size_t outlen) {
    uint8_t v[128];
    size_t hl = strlen(hrp), n = 0;
    if (hl > 10 || plen > 40) return -1;
    for (size_t i = 0; i < hl; i++) v[n++] = hrp[i] >> 5;
    v[n++] = 0;
    for (size_t i = 0; i < hl; i++) v[n++] = hrp[i] & 31;
    size_t data_start = n;
    v[n++] = (uint8_t)witver;
    uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < plen; i++) {
        acc = (acc << 8) | prog[i]; bits += 8;
        while (bits >= 5) { bits -= 5; v[n++] = (acc >> bits) & 31; }
    }
    if (bits) v[n++] = (acc << (5 - bits)) & 31;
    size_t data_end = n;
    for (int i = 0; i < 6; i++) v[n++] = 0;
    uint32_t constv = witver == 0 ? 1 : 0x2bc830a3;
    uint32_t mod = polymod(v, n) ^ constv;
    for (int i = 0; i < 6; i++) v[data_end + i] = (mod >> (5 * (5 - i))) & 31;
    if (hl + 1 + (data_end + 6 - data_start) + 1 > outlen) return -1;
    size_t o = 0;
    memcpy(out, hrp, hl); o = hl;
    out[o++] = '1';
    for (size_t i = data_start; i < data_end + 6; i++) out[o++] = B32[v[i]];
    out[o] = 0;
    return 0;
}

int bs_script_to_addr(const uint8_t *s, size_t n, int testnet, char *out, size_t outlen) {
    const char *hrp = testnet ? "tb" : "bc";
    uint8_t buf[21];
    if (n == 22 && s[0] == 0x00 && s[1] == 20) return bs_segwit_addr(hrp, 0, s + 2, 20, out, outlen);
    if (n == 34 && s[0] == 0x00 && s[1] == 32) return bs_segwit_addr(hrp, 0, s + 2, 32, out, outlen);
    if (n >= 4 && n <= 42 && s[0] >= 0x51 && s[0] <= 0x60 && s[1] == n - 2)
        return bs_segwit_addr(hrp, s[0] - 0x50, s + 2, n - 2, out, outlen);
    if (n == 25 && s[0] == 0x76 && s[1] == 0xa9 && s[2] == 20 && s[23] == 0x88 && s[24] == 0xac) {
        buf[0] = testnet ? 0x6f : 0x00; memcpy(buf + 1, s + 3, 20);
        return bs_base58check(buf, 21, out, outlen);
    }
    if (n == 23 && s[0] == 0xa9 && s[1] == 20 && s[22] == 0x87) {
        buf[0] = testnet ? 0xc4 : 0x05; memcpy(buf + 1, s + 2, 20);
        return bs_base58check(buf, 21, out, outlen);
    }
    return -1;  // non-standard (e.g. OP_RETURN)
}
