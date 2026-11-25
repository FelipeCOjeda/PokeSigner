// PokeSigner core: BIP32 + PSBT v0 (BIP174) signing for P2WPKH and P2TR key-path.
#include "bs_signer.h"
#include "bs_crypto.h"
#include "bs_encode.h"
#include "secp256k1.h"
#include "secp256k1_extrakeys.h"
#include "secp256k1_schnorrsig.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#define H(x) ((x) | 0x80000000u)

static secp256k1_context *g_ctx;
static uint8_t g_mpriv[32], g_mchain[32];
static uint32_t g_fp;
static int g_ready;

// diagnostics for "nothing to sign"
static char g_detail[120];
static uint32_t g_seen_fp;
static int g_seen_any, g_seen_zero, g_seen_deriv;

const char *bs_psbt_detail(void) { return g_detail; }
int bs_psbt_foreign_fp(uint32_t *fp) { if (g_seen_any) *fp = g_seen_fp; return g_seen_any; }
int bs_psbt_zero_fp(void) { return g_seen_zero; }

const char *bs_strerror(int e) {
    switch (e) {
    case BS_OK: return "ok";
    case BS_ERR_PARSE: return "PSBT invalido";
    case BS_ERR_UNSUPPORTED: return "recurso nao suportado (PSBTv2, sighash, script)";
    case BS_ERR_NOTHING_TO_SIGN: return "nenhuma entrada pertence a esta seed";
    case BS_ERR_MISMATCH: return "derivacao nao confere com o script";
    case BS_ERR_MISSING_UTXO: return "UTXO ausente/inconsistente";
    case BS_ERR_BUFFER: return "buffer insuficiente";
    case BS_ERR_CRYPTO: return "erro criptografico";
    case BS_ERR_NETWORK: return "mistura de redes";
    default: return "erro";
    }
}

/* ------------------------------------------------------------------ BIP32 */

static int ckd_priv(uint8_t priv[32], uint8_t chain[32], uint32_t idx) {
    uint8_t data[37], I[64];
    if (idx & 0x80000000u) {
        data[0] = 0; memcpy(data + 1, priv, 32);
    } else {
        secp256k1_pubkey pk; size_t l = 33;
        if (!secp256k1_ec_pubkey_create(g_ctx, &pk, priv)) return -1;
        secp256k1_ec_pubkey_serialize(g_ctx, data, &l, &pk, SECP256K1_EC_COMPRESSED);
    }
    data[33] = idx >> 24; data[34] = idx >> 16; data[35] = idx >> 8; data[36] = idx;
    bs_hmac_sha512(chain, 32, data, 37, I);
    int ok = secp256k1_ec_seckey_verify(g_ctx, I) && secp256k1_ec_seckey_tweak_add(g_ctx, priv, I);
    memcpy(chain, I + 32, 32);
    bs_wipe(I, sizeof(I)); bs_wipe(data, sizeof(data));
    return ok ? 0 : -1;
}

static int pub_from_priv(const uint8_t priv[32], uint8_t pub[33]) {
    secp256k1_pubkey pk; size_t l = 33;
    if (!secp256k1_ec_pubkey_create(g_ctx, &pk, priv)) return -1;
    secp256k1_ec_pubkey_serialize(g_ctx, pub, &l, &pk, SECP256K1_EC_COMPRESSED);
    return 0;
}

// Derives priv/chain at path. parent_fp (optional) receives fingerprint of the parent.
static int derive(const uint32_t *path, size_t depth, uint8_t priv[32], uint8_t chain[32], uint32_t *parent_fp) {
    memcpy(priv, g_mpriv, 32); memcpy(chain, g_mchain, 32);
    for (size_t i = 0; i < depth; i++) {
        if (parent_fp && i == depth - 1) {
            uint8_t pub[33], h[20];
            if (pub_from_priv(priv, pub)) return -1;
            bs_hash160(pub, 33, h);
            *parent_fp = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
        }
        if (ckd_priv(priv, chain, path[i])) return -1;
    }
    return 0;
}

int bs_signer_init(const uint8_t seed[64]) {
    if (!g_ctx) {
        uint8_t r[32];
        g_ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        if (!g_ctx) return BS_ERR_CRYPTO;
        bs_platform_random(r, 32);
        if (!secp256k1_context_randomize(g_ctx, r)) return BS_ERR_CRYPTO;
        bs_wipe(r, 32);
    }
    uint8_t I[64], pub[33], h[20];
    bs_hmac_sha512((const uint8_t *)"Bitcoin seed", 12, seed, 64, I);
    if (!secp256k1_ec_seckey_verify(g_ctx, I)) { bs_wipe(I, 64); return BS_ERR_CRYPTO; }
    memcpy(g_mpriv, I, 32); memcpy(g_mchain, I + 32, 32);
    bs_wipe(I, 64);
    if (pub_from_priv(g_mpriv, pub)) return BS_ERR_CRYPTO;
    bs_hash160(pub, 33, h);
    g_fp = (uint32_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
    g_ready = 1;
    return BS_OK;
}

void bs_signer_wipe(void) {
    bs_wipe(g_mpriv, 32); bs_wipe(g_mchain, 32); g_fp = 0; g_ready = 0;
}

uint32_t bs_signer_fingerprint(void) { return g_fp; }

/* --------------------------------------------------- descriptors / info */

static uint64_t descsum_polymod(uint64_t c, int val) {
    uint8_t c0 = c >> 35;
    c = ((c & 0x7ffffffffULL) << 5) ^ (uint64_t)val;
    if (c0 & 1) c ^= 0xf5dee51989ULL;
    if (c0 & 2) c ^= 0xa9fdca3312ULL;
    if (c0 & 4) c ^= 0x1bab10e32dULL;
    if (c0 & 8) c ^= 0x3706b1677aULL;
    if (c0 & 16) c ^= 0x644d626ffdULL;
    return c;
}

static int descsum(const char *s, char out[9]) {
    static const char *IN = "0123456789()[],'/*abcdefgh@:$%{}"
                            "IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~"
                            "ijklmnopqrstuvwxyzABCDEFGH`#\"\\ ";
    static const char *CS = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    uint64_t c = 1; int cls = 0, clscount = 0;
    for (; *s; s++) {
        const char *p = strchr(IN, *s);
        if (!p) return -1;
        int pos = (int)(p - IN);
        c = descsum_polymod(c, pos & 31);
        cls = cls * 3 + (pos >> 5);
        if (++clscount == 3) { c = descsum_polymod(c, cls); cls = 0; clscount = 0; }
    }
    if (clscount > 0) c = descsum_polymod(c, cls);
    for (int j = 0; j < 8; j++) c = descsum_polymod(c, 0);
    c ^= 1;
    for (int j = 0; j < 8; j++) out[j] = CS[(c >> (5 * (7 - j))) & 31];
    out[8] = 0;
    return 0;
}

static int xpub_at(const uint32_t *path, size_t depth, int testnet, char *out, size_t outlen) {
    uint8_t priv[32], chain[32], ser[78];
    uint32_t pfp = 0, ver = testnet ? 0x043587CF : 0x0488B21E, child = path[depth - 1];
    if (derive(path, depth, priv, chain, &pfp)) return BS_ERR_CRYPTO;
    ser[0] = ver >> 24; ser[1] = ver >> 16; ser[2] = ver >> 8; ser[3] = ver;
    ser[4] = (uint8_t)depth;
    ser[5] = pfp >> 24; ser[6] = pfp >> 16; ser[7] = pfp >> 8; ser[8] = pfp;
    ser[9] = child >> 24; ser[10] = child >> 16; ser[11] = child >> 8; ser[12] = child;
    memcpy(ser + 13, chain, 32);
    int r = pub_from_priv(priv, ser + 45);
    bs_wipe(priv, 32);
    if (r) return BS_ERR_CRYPTO;
    return bs_base58check(ser, 78, out, outlen) ? BS_ERR_BUFFER : BS_OK;
}

int bs_signer_get_info(int testnet, uint32_t account, char *out, size_t outlen) {
    if (!g_ready) return BS_ERR_CRYPTO;
    static const struct { uint32_t purpose; const char *name; } T[2] = {{84, "wpkh"}, {86, "tr"}};
    uint32_t coin = testnet ? 1 : 0;
    int n = snprintf(out, outlen, "{\"fingerprint\":\"%08" PRIx32 "\",\"network\":\"%s\",\"accounts\":[",
                     g_fp, testnet ? "test" : "main");
    for (int t = 0; t < 2; t++) {
        uint32_t path[3] = {H(T[t].purpose), H(coin), H(account)};
        char xpub[120], origin[64], d[2][300], cs[2][9];
        if (xpub_at(path, 3, testnet, xpub, sizeof(xpub))) return BS_ERR_CRYPTO;
        snprintf(origin, sizeof(origin), "%08" PRIx32 "/%" PRIu32 "h/%" PRIu32 "h/%" PRIu32 "h",
                 g_fp, T[t].purpose, coin, account);
        for (int c = 0; c < 2; c++) {
            snprintf(d[c], sizeof(d[c]), "%s([%s]%s/%d/*)", T[t].name, origin, xpub, c);
            if (descsum(d[c], cs[c])) return BS_ERR_CRYPTO;
        }
        n += snprintf(out + n, n < (int)outlen ? outlen - n : 0,
                      "%s{\"type\":\"%s\",\"path\":\"m/%" PRIu32 "h/%" PRIu32 "h/%" PRIu32 "h\",\"xpub\":\"%s\","
                      "\"receive\":\"%s#%s\",\"change\":\"%s#%s\"}",
                      t ? "," : "", T[t].name, T[t].purpose, coin, account, xpub, d[0], cs[0], d[1], cs[1]);
    }
    n += snprintf(out + n, n < (int)outlen ? outlen - n : 0, "]}");
    return (n < (int)outlen) ? BS_OK : BS_ERR_BUFFER;
}

/* ------------------------------------------------------------ reader */

typedef struct { const uint8_t *p; size_t n, i; int err; } rd_t;

static const uint8_t *rd_bytes(rd_t *r, uint64_t len) {
    if (r->err || len > r->n - r->i) { r->err = 1; return NULL; }
    const uint8_t *q = r->p + r->i; r->i += (size_t)len; return q;
}
static uint64_t rd_le(rd_t *r, int len) {
    const uint8_t *q = rd_bytes(r, len); uint64_t v = 0;
    if (!q) return 0;
    for (int i = len - 1; i >= 0; i--) v = (v << 8) | q[i];
    return v;
}
static uint64_t rd_varint(rd_t *r) {
    uint64_t v = rd_le(r, 1);
    if (v == 0xfd) v = rd_le(r, 2);
    else if (v == 0xfe) v = rd_le(r, 4);
    else if (v == 0xff) v = rd_le(r, 8);
    return v;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// value = fingerprint(4) || path (4*k). Returns 1 if fingerprint is ours and path fits.
static int parse_keypath(const uint8_t *v, size_t vlen, uint32_t *path, size_t *depth) {
    if (vlen < 4 || (vlen - 4) % 4 || (vlen - 4) / 4 > BS_MAX_PATH) return 0;
    g_seen_deriv = 1;
    if (be32(v) == 0) g_seen_zero = 1;
    else if (be32(v) != g_fp && !g_seen_any) { g_seen_fp = be32(v); g_seen_any = 1; }
    if (be32(v) != g_fp) return 0;
    *depth = (vlen - 4) / 4;
    for (size_t i = 0; i < *depth; i++) path[i] = le32(v + 4 + 4 * i);
    return 1;
}

/* ----------------------------------------------------- tx helpers */

// Locates output `vout` in a full network-serialized tx and computes its txid.
static int tx_get_output(const uint8_t *tx, size_t len, uint32_t vout, uint8_t txid[32],
                         uint64_t *amount, const uint8_t **spk, size_t *spk_len) {
    rd_t r = {tx, len, 0, 0};
    rd_bytes(&r, 4);
    int segwit = 0;
    if (r.i + 2 <= len && tx[4] == 0x00 && tx[5] == 0x01) { segwit = 1; r.i += 2; }
    size_t body_start = r.i;
    uint64_t nin = rd_varint(&r);
    if (nin > 10000) return -1;
    for (uint64_t i = 0; i < nin && !r.err; i++) {
        rd_bytes(&r, 36); rd_bytes(&r, rd_varint(&r)); rd_bytes(&r, 4);
    }
    uint64_t nout = rd_varint(&r);
    if (nout > 10000 || vout >= nout) return -1;
    for (uint64_t i = 0; i < nout && !r.err; i++) {
        uint64_t a = rd_le(&r, 8);
        uint64_t sl = rd_varint(&r);
        const uint8_t *s = rd_bytes(&r, sl);
        if (i == vout) { *amount = a; *spk = s; *spk_len = sl; }
    }
    size_t body_end = r.i;
    if (segwit) {
        for (uint64_t i = 0; i < nin && !r.err; i++) {
            uint64_t items = rd_varint(&r);
            for (uint64_t k = 0; k < items && !r.err; k++) rd_bytes(&r, rd_varint(&r));
        }
    }
    const uint8_t *lt = rd_bytes(&r, 4);
    if (r.err || r.i != len) return -1;
    bs_sha256_ctx c; uint8_t h[32];
    bs_sha256_init(&c);
    bs_sha256_update(&c, tx, 4);
    bs_sha256_update(&c, tx + body_start, body_end - body_start);
    bs_sha256_update(&c, lt, 4);
    bs_sha256_final(&c, h);
    bs_sha256(h, 32, txid);
    return 0;
}

static int is_p2wpkh(const uint8_t *s, size_t n) { return n == 22 && s[0] == 0x00 && s[1] == 20; }
static int is_p2tr(const uint8_t *s, size_t n) { return n == 34 && s[0] == 0x51 && s[1] == 32; }

// Output key for BIP86/BIP341: P + H_TapTweak(P || root)G. Writes xonly(Q).
static int taproot_output_key(const uint8_t internal[32], const uint8_t *root, uint8_t outkey[32]) {
    secp256k1_xonly_pubkey xp, xq; secp256k1_pubkey q; uint8_t t[32]; int parity;
    bs_sha256_ctx c;
    if (!secp256k1_xonly_pubkey_parse(g_ctx, &xp, internal)) return -1;
    bs_tagged_init(&c, "TapTweak");
    bs_sha256_update(&c, internal, 32);
    if (root) bs_sha256_update(&c, root, 32);
    bs_sha256_final(&c, t);
    if (!secp256k1_xonly_pubkey_tweak_add(g_ctx, &q, &xp, t)) return -1;
    if (!secp256k1_xonly_pubkey_from_pubkey(g_ctx, &xq, &parity, &q)) return -1;
    secp256k1_xonly_pubkey_serialize(g_ctx, outkey, &xq);
    return 0;
}

// Derives the compressed pubkey at path.
static int pub_at(const uint32_t *path, size_t depth, uint8_t pub[33]) {
    uint8_t priv[32], chain[32];
    int r = derive(path, depth, priv, chain, NULL) || pub_from_priv(priv, pub);
    bs_wipe(priv, 32); bs_wipe(chain, 32);
    return r ? -1 : 0;
}

static int path_network(const uint32_t *path, size_t depth) {
    if (depth >= 2 && path[1] == H(1)) return 1;
    if (depth >= 2 && path[1] == H(0)) return 0;
    return -1;
}

/* ----------------------------------------------------- PSBT analyze */

int bs_psbt_analyze(const uint8_t *buf, size_t len, bs_psbt_t *p) {
    if (!g_ready) return BS_ERR_CRYPTO;
    memset(p, 0, sizeof(*p));
    p->buf = buf; p->len = len; p->testnet = -1;
    g_detail[0] = 0; g_seen_fp = 0; g_seen_any = g_seen_zero = g_seen_deriv = 0;
    rd_t r = {buf, len, 0, 0};
    const uint8_t *magic = rd_bytes(&r, 5);
    if (!magic || memcmp(magic, "psbt\xff", 5)) return BS_ERR_PARSE;

    /* global map */
    for (;;) {
        uint64_t kl = rd_varint(&r);
        if (r.err) return BS_ERR_PARSE;
        if (kl == 0) break;
        const uint8_t *k = rd_bytes(&r, kl);
        uint64_t vl = rd_varint(&r);
        const uint8_t *v = rd_bytes(&r, vl);
        if (r.err) return BS_ERR_PARSE;
        if (k[0] == 0x00 && kl == 1) { p->tx = v; p->tx_len = vl; }
        if (k[0] == 0xfb && kl == 1) { if (vl != 4 || le32(v) != 0) return BS_ERR_UNSUPPORTED; }
    }
    p->global_end = r.i - 1;
    if (!p->tx) return BS_ERR_UNSUPPORTED;  // PSBTv2 or broken

    /* unsigned tx */
    rd_t t = {p->tx, p->tx_len, 0, 0};
    p->tx_version = (uint32_t)rd_le(&t, 4);
    uint64_t nin = rd_varint(&t);
    if (nin == 0 || nin > BS_MAX_INPUTS) return nin ? BS_ERR_UNSUPPORTED : BS_ERR_PARSE;
    p->n_in = nin;
    for (size_t i = 0; i < nin; i++) {
        p->in[i].prevout = rd_bytes(&t, 36);
        if (rd_varint(&t) != 0) return BS_ERR_PARSE;  // scriptSig must be empty
        p->in[i].sequence = (uint32_t)rd_le(&t, 4);
    }
    uint64_t nout = rd_varint(&t);
    if (nout > BS_MAX_OUTPUTS) return BS_ERR_UNSUPPORTED;
    p->n_out = nout;
    for (size_t i = 0; i < nout; i++) {
        p->out[i].amount = rd_le(&t, 8);
        p->out[i].spk_len = rd_varint(&t);
        p->out[i].spk = rd_bytes(&t, p->out[i].spk_len);
    }
    p->locktime = (uint32_t)rd_le(&t, 4);
    if (t.err || t.i != t.n) return BS_ERR_PARSE;

    /* inputs */
    int all_utxo = 1;
    for (size_t i = 0; i < p->n_in; i++) {
        bs_input_t *in = &p->in[i];
        in->map_start = r.i; in->sighash = -1;
        uint8_t ecdsa_pub[33]; uint32_t epath[BS_MAX_PATH]; size_t edepth = 0; int has_e = 0;
        uint8_t tap_x[32]; uint32_t tpath[BS_MAX_PATH]; size_t tdepth = 0; int has_t = 0;
        const uint8_t *tap_internal = NULL, *nwu = NULL; size_t nwu_len = 0;
        int has_tap_sig = 0;
        const uint8_t *psig_keys[8]; int n_psig = 0;
        for (;;) {
            uint64_t kl = rd_varint(&r);
            if (r.err) return BS_ERR_PARSE;
            if (kl == 0) break;
            const uint8_t *k = rd_bytes(&r, kl);
            uint64_t vl = rd_varint(&r);
            const uint8_t *v = rd_bytes(&r, vl);
            if (r.err) return BS_ERR_PARSE;
            switch (k[0]) {
            case 0x00: if (kl == 1) { nwu = v; nwu_len = vl; } break;
            case 0x01:
                if (kl == 1) {
                    rd_t w = {v, vl, 0, 0};
                    in->amount = rd_le(&w, 8);
                    in->spk_len = rd_varint(&w);
                    in->spk = rd_bytes(&w, in->spk_len);
                    if (w.err || w.i != vl) return BS_ERR_PARSE;
                    in->has_utxo = 1;
                }
                break;
            case 0x02: if (kl == 34 && n_psig < 8) psig_keys[n_psig++] = k + 1; break;
            case 0x03: if (kl == 1 && vl == 4) in->sighash = (int)le32(v); break;
            case 0x06:
                if (kl == 34 && !has_e && parse_keypath(v, vl, epath, &edepth)) {
                    memcpy(ecdsa_pub, k + 1, 33); has_e = 1;
                }
                break;
            case 0x13: if (kl == 1) has_tap_sig = 1; break;
            case 0x16:
                if (kl == 33 && !has_t) {
                    rd_t w = {v, vl, 0, 0};
                    uint64_t nh = rd_varint(&w);
                    if (w.err) return BS_ERR_PARSE;
                    if (nh == 0 && parse_keypath(v + w.i, vl - w.i, tpath, &tdepth)) {
                        memcpy(tap_x, k + 1, 32); has_t = 1;
                    }
                }
                break;
            case 0x17: if (kl == 1 && vl == 32) tap_internal = v; break;
            case 0x18: if (kl == 1 && vl == 32) in->merkle_root = v; break;
            default: break;
            }
        }
        in->map_end = r.i - 1;

        if (nwu) {
            uint8_t txid[32]; uint64_t a = 0; const uint8_t *s = NULL; size_t sl = 0;
            if (tx_get_output(nwu, nwu_len, le32(in->prevout + 32), txid, &a, &s, &sl))
                return BS_ERR_MISSING_UTXO;
            if (memcmp(txid, in->prevout, 32)) return BS_ERR_MISSING_UTXO;
            if (in->has_utxo && (a != in->amount || sl != in->spk_len || memcmp(s, in->spk, sl)))
                return BS_ERR_MISSING_UTXO;
            in->amount = a; in->spk = s; in->spk_len = sl; in->has_utxo = 1;
        }
        if (!in->has_utxo) { all_utxo = 0; continue; }
        p->in_total += in->amount;

        if (is_p2wpkh(in->spk, in->spk_len) && has_e) {
            uint8_t pub[33], h[20];
            if (pub_at(epath, edepth, pub)) return BS_ERR_CRYPTO;
            bs_hash160(pub, 33, h);
            if (memcmp(pub, ecdsa_pub, 33) || memcmp(h, in->spk + 2, 20)) return BS_ERR_MISMATCH;
            int already = 0;
            for (int s = 0; s < n_psig; s++) if (!memcmp(psig_keys[s], pub, 33)) already = 1;
            if (in->sighash != -1 && in->sighash != 1) return BS_ERR_UNSUPPORTED;
            if (!already) { in->sign_type = 1; memcpy(in->path, epath, sizeof(epath)); in->depth = edepth; }
        } else if (is_p2tr(in->spk, in->spk_len) && has_t) {
            uint8_t pub[33], q[32];
            if (pub_at(tpath, tdepth, pub)) return BS_ERR_CRYPTO;
            if (memcmp(pub + 1, tap_x, 32)) return BS_ERR_MISMATCH;
            if (tap_internal && memcmp(tap_internal, tap_x, 32)) return BS_ERR_MISMATCH;
            if (taproot_output_key(tap_x, in->merkle_root, q)) return BS_ERR_CRYPTO;
            if (memcmp(q, in->spk + 2, 32)) return BS_ERR_MISMATCH;
            if (in->sighash != -1 && in->sighash != 0 && in->sighash != 1) return BS_ERR_UNSUPPORTED;
            if (!has_tap_sig) { in->sign_type = 2; memcpy(in->path, tpath, sizeof(tpath)); in->depth = tdepth; }
        }
        if (in->sign_type) {
            int net = path_network(in->path, in->depth);
            if (net >= 0) {
                if (p->testnet >= 0 && p->testnet != net) return BS_ERR_NETWORK;
                p->testnet = net;
            }
            p->n_sign++;
        }
    }

    /* outputs */
    for (size_t i = 0; i < p->n_out; i++) {
        bs_output_t *o = &p->out[i];
        o->map_start = r.i;
        uint8_t epub[33]; uint32_t epath[BS_MAX_PATH]; size_t edepth = 0; int has_e = 0;
        uint8_t tx[32]; uint32_t tpath[BS_MAX_PATH]; size_t tdepth = 0; int has_t = 0;
        for (;;) {
            uint64_t kl = rd_varint(&r);
            if (r.err) return BS_ERR_PARSE;
            if (kl == 0) break;
            const uint8_t *k = rd_bytes(&r, kl);
            uint64_t vl = rd_varint(&r);
            const uint8_t *v = rd_bytes(&r, vl);
            if (r.err) return BS_ERR_PARSE;
            if (k[0] == 0x02 && kl == 34 && !has_e && parse_keypath(v, vl, epath, &edepth)) {
                memcpy(epub, k + 1, 33); has_e = 1;
            } else if (k[0] == 0x07 && kl == 33 && !has_t) {
                rd_t w = {v, vl, 0, 0};
                uint64_t nh = rd_varint(&w);
                if (w.err) return BS_ERR_PARSE;
                if (nh == 0 && parse_keypath(v + w.i, vl - w.i, tpath, &tdepth)) { memcpy(tx, k + 1, 32); has_t = 1; }
            }
        }
        o->map_end = r.i - 1;
        uint8_t pub[33];
        if (has_e && is_p2wpkh(o->spk, o->spk_len) && !pub_at(epath, edepth, pub) && !memcmp(pub, epub, 33)) {
            uint8_t h[20]; bs_hash160(pub, 33, h);
            if (!memcmp(h, o->spk + 2, 20) && path_network(epath, edepth) == p->testnet) o->is_change = 1;
        }
        if (!o->is_change && has_t && is_p2tr(o->spk, o->spk_len) && !pub_at(tpath, tdepth, pub) &&
            !memcmp(pub + 1, tx, 32)) {
            uint8_t q[32];
            if (!taproot_output_key(tx, NULL, q) && !memcmp(q, o->spk + 2, 32) &&
                path_network(tpath, tdepth) == p->testnet) o->is_change = 1;
        }
        p->out_total += o->amount;
        if (o->is_change) p->change_total += o->amount;
    }
    if (r.err) return BS_ERR_PARSE;

    if (p->n_sign == 0) {
        if (g_seen_any)
            snprintf(g_detail, sizeof(g_detail), "PSBT e da carteira %08" PRIx32 ", este signer e %08" PRIx32
                     " (outra seed ou passphrase)", g_seen_fp, g_fp);
        else if (g_seen_zero)
            snprintf(g_detail, sizeof(g_detail), "PSBT sem fingerprint (carteira importada so pela xpub); "
                     "importe pelo PokeSigner ou com [fingerprint/caminho]xpub");
        else if (!g_seen_deriv)
            snprintf(g_detail, sizeof(g_detail), "PSBT sem dados de derivacao BIP32 nas entradas");
        else if (!all_utxo)
            snprintf(g_detail, sizeof(g_detail), "entradas sem witness_utxo");
        return BS_ERR_NOTHING_TO_SIGN;
    }
    if (p->testnet < 0) p->testnet = 0;
    for (size_t i = 0; i < p->n_in; i++)
        if (p->in[i].sign_type == 2 && !all_utxo) return BS_ERR_MISSING_UTXO;  // BIP341 needs all prevouts
    if (all_utxo) {
        if (p->out_total > p->in_total) return BS_ERR_PARSE;
        p->fee = p->in_total - p->out_total; p->fee_known = 1;
    }
    return BS_OK;
}

/* ----------------------------------------------------- summary */

static void fmt_btc(uint64_t sats, char *out, size_t n) {
    snprintf(out, n, "%" PRIu64 ".%08" PRIu64 " BTC", (uint64_t)(sats / 100000000u), (uint64_t)(sats % 100000000u));
}

int bs_psbt_summary(const bs_psbt_t *p, char *out, size_t outlen) {
    size_t n = 0; char a[32];
#define PUT(...) do { int w = snprintf(out + n, n < outlen ? outlen - n : 0, __VA_ARGS__); if (w > 0) n += w; } while (0)
    PUT("==== CONFIRMAR TRANSACAO [%s] ====\n", p->testnet ? "TESTNET" : "MAINNET");
    PUT("Entradas: %u (assinando %u)\n", (unsigned)p->n_in, (unsigned)p->n_sign);
    unsigned k = 0;
    for (size_t i = 0; i < p->n_out; i++) {
        const bs_output_t *o = &p->out[i];
        if (o->is_change) continue;
        char addr[100];
        fmt_btc(o->amount, a, sizeof(a));
        if (bs_script_to_addr(o->spk, o->spk_len, p->testnet, addr, sizeof(addr)) == 0)
            PUT("Destino %u: %s\n           %s\n", ++k, addr, a);
        else if (o->spk_len && o->spk[0] == 0x6a)
            PUT("Destino %u: OP_RETURN (%u bytes)  %s\n", ++k, (unsigned)o->spk_len, a);
        else
            PUT("Destino %u: script nao padrao  %s\n", ++k, a);
    }
    if (k == 0) PUT("Destino: nenhum (auto-transferencia)\n");
    fmt_btc(p->change_total, a, sizeof(a));
    PUT("Troco (verificado): %s\n", a);
    if (p->fee_known) {
        fmt_btc(p->fee, a, sizeof(a));
        PUT("Taxa: %s\n", a);
        uint64_t sent = p->out_total - p->change_total;
        if (p->fee > 1000000ULL || (sent && p->fee * 10 > sent))
            PUT("!!! ATENCAO: TAXA ALTA !!!\n");
    } else {
        PUT("Taxa: DESCONHECIDA (faltam UTXOs de entradas de terceiros)\n");
    }
    if (p->locktime) PUT("Locktime: %" PRIu32 "\n", p->locktime);
#undef PUT
    return n < outlen ? BS_OK : BS_ERR_BUFFER;
}

/* ----------------------------------------------------- sighash + sign */

static void put_varint_hash(bs_sha256_ctx *c, uint64_t v) {
    uint8_t b[9]; size_t l;
    if (v < 0xfd) { b[0] = (uint8_t)v; l = 1; }
    else if (v <= 0xffff) { b[0] = 0xfd; b[1] = v; b[2] = v >> 8; l = 3; }
    else { b[0] = 0xfe; b[1] = v; b[2] = v >> 8; b[3] = v >> 16; b[4] = v >> 24; l = 5; }
    bs_sha256_update(c, b, l);
}
static void u32le(uint8_t *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }
static void u64le(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i)); }

typedef struct { uint8_t prevouts[32], sequences[32], outputs[32], amounts[32], spks[32]; } sh_cache_t;

static void build_cache(const bs_psbt_t *p, sh_cache_t *s) {
    bs_sha256_ctx c; uint8_t b[8];
    bs_sha256_init(&c);
    for (size_t i = 0; i < p->n_in; i++) bs_sha256_update(&c, p->in[i].prevout, 36);
    bs_sha256_final(&c, s->prevouts);
    bs_sha256_init(&c);
    for (size_t i = 0; i < p->n_in; i++) { u32le(b, p->in[i].sequence); bs_sha256_update(&c, b, 4); }
    bs_sha256_final(&c, s->sequences);
    bs_sha256_init(&c);
    for (size_t i = 0; i < p->n_out; i++) {
        u64le(b, p->out[i].amount); bs_sha256_update(&c, b, 8);
        put_varint_hash(&c, p->out[i].spk_len); bs_sha256_update(&c, p->out[i].spk, p->out[i].spk_len);
    }
    bs_sha256_final(&c, s->outputs);
    bs_sha256_init(&c);
    for (size_t i = 0; i < p->n_in; i++) { u64le(b, p->in[i].amount); bs_sha256_update(&c, b, 8); }
    bs_sha256_final(&c, s->amounts);
    bs_sha256_init(&c);
    for (size_t i = 0; i < p->n_in; i++) {
        put_varint_hash(&c, p->in[i].spk_len); bs_sha256_update(&c, p->in[i].spk, p->in[i].spk_len);
    }
    bs_sha256_final(&c, s->spks);
}

static void sighash_bip143(const bs_psbt_t *p, const sh_cache_t *s, size_t idx, uint8_t out[32]) {
    const bs_input_t *in = &p->in[idx];
    uint8_t hp[32], hs[32], ho[32], b[8];
    bs_sha256(s->prevouts, 32, hp); bs_sha256(s->sequences, 32, hs); bs_sha256(s->outputs, 32, ho);
    bs_sha256_ctx c; bs_sha256_init(&c);
    u32le(b, p->tx_version); bs_sha256_update(&c, b, 4);
    bs_sha256_update(&c, hp, 32);
    bs_sha256_update(&c, hs, 32);
    bs_sha256_update(&c, in->prevout, 36);
    uint8_t sc[26] = {0x19, 0x76, 0xa9, 0x14};
    memcpy(sc + 4, in->spk + 2, 20); sc[24] = 0x88; sc[25] = 0xac;
    bs_sha256_update(&c, sc, 26);
    u64le(b, in->amount); bs_sha256_update(&c, b, 8);
    u32le(b, in->sequence); bs_sha256_update(&c, b, 4);
    bs_sha256_update(&c, ho, 32);
    u32le(b, p->locktime); bs_sha256_update(&c, b, 4);
    u32le(b, 1); bs_sha256_update(&c, b, 4);  // SIGHASH_ALL
    uint8_t h1[32]; bs_sha256_final(&c, h1);
    bs_sha256(h1, 32, out);
}

static void sighash_bip341(const bs_psbt_t *p, const sh_cache_t *s, size_t idx, uint8_t hash_type, uint8_t out[32]) {
    bs_sha256_ctx c; uint8_t b[4];
    bs_tagged_init(&c, "TapSighash");
    uint8_t hdr[2] = {0x00, hash_type};
    bs_sha256_update(&c, hdr, 2);
    u32le(b, p->tx_version); bs_sha256_update(&c, b, 4);
    u32le(b, p->locktime); bs_sha256_update(&c, b, 4);
    bs_sha256_update(&c, s->prevouts, 32);
    bs_sha256_update(&c, s->amounts, 32);
    bs_sha256_update(&c, s->spks, 32);
    bs_sha256_update(&c, s->sequences, 32);
    bs_sha256_update(&c, s->outputs, 32);
    uint8_t spend_type = 0; bs_sha256_update(&c, &spend_type, 1);
    u32le(b, (uint32_t)idx); bs_sha256_update(&c, b, 4);
    bs_sha256_final(&c, out);
}

static int emit(uint8_t *out, size_t cap, size_t *n, const void *src, size_t len) {
    if (len > cap - *n) return -1;
    memcpy(out + *n, src, len); *n += len; return 0;
}

int bs_psbt_sign(const bs_psbt_t *p, uint8_t *out, size_t cap, size_t *outlen) {
    sh_cache_t sc;
    build_cache(p, &sc);
    size_t n = 0;
    if (emit(out, cap, &n, p->buf, p->global_end + 1)) return BS_ERR_BUFFER;
    int rc = BS_OK;
    for (size_t i = 0; i < p->n_in && rc == BS_OK; i++) {
        const bs_input_t *in = &p->in[i];
        if (emit(out, cap, &n, p->buf + in->map_start, in->map_end - in->map_start)) return BS_ERR_BUFFER;
        if (in->sign_type) {
            uint8_t priv[32], chain[32], msg[32], pair[1 + 1 + 33 + 1 + 73];
            size_t pl = 0;
            if (derive(in->path, in->depth, priv, chain, NULL)) return BS_ERR_CRYPTO;
            if (in->sign_type == 1) {
                secp256k1_ecdsa_signature sig; uint8_t der[72]; size_t dl = sizeof(der);
                sighash_bip143(p, &sc, i, msg);
                pair[pl++] = 34; pair[pl++] = 0x02;
                if (pub_from_priv(priv, pair + pl)) rc = BS_ERR_CRYPTO;
                pl += 33;
                if (rc == BS_OK && (!secp256k1_ecdsa_sign(g_ctx, &sig, msg, priv, NULL, NULL) ||
                                    !secp256k1_ecdsa_signature_serialize_der(g_ctx, der, &dl, &sig)))
                    rc = BS_ERR_CRYPTO;
                if (rc == BS_OK) {
                    pair[pl++] = (uint8_t)(dl + 1);
                    memcpy(pair + pl, der, dl); pl += dl;
                    pair[pl++] = 0x01;
                }
            } else {
                secp256k1_keypair kp; uint8_t t[32], x[32], aux[32], sig[64];
                secp256k1_xonly_pubkey xp; bs_sha256_ctx c;
                uint8_t ht = in->sighash == 1 ? 1 : 0;
                sighash_bip341(p, &sc, i, ht, msg);
                if (!secp256k1_keypair_create(g_ctx, &kp, priv) ||
                    !secp256k1_keypair_xonly_pub(g_ctx, &xp, NULL, &kp)) rc = BS_ERR_CRYPTO;
                if (rc == BS_OK) {
                    secp256k1_xonly_pubkey_serialize(g_ctx, x, &xp);
                    bs_tagged_init(&c, "TapTweak");
                    bs_sha256_update(&c, x, 32);
                    if (in->merkle_root) bs_sha256_update(&c, in->merkle_root, 32);
                    bs_sha256_final(&c, t);
                    bs_platform_random(aux, 32);
                    if (!secp256k1_keypair_xonly_tweak_add(g_ctx, &kp, t) ||
                        !secp256k1_schnorrsig_sign32(g_ctx, sig, msg, &kp, aux)) rc = BS_ERR_CRYPTO;
                    // self-check against the output key
                    secp256k1_xonly_pubkey q;
                    if (rc == BS_OK && (!secp256k1_keypair_xonly_pub(g_ctx, &q, NULL, &kp) ||
                                        !secp256k1_schnorrsig_verify(g_ctx, sig, msg, 32, &q)))
                        rc = BS_ERR_CRYPTO;
                }
                if (rc == BS_OK) {
                    pair[pl++] = 1; pair[pl++] = 0x13;
                    pair[pl++] = ht ? 65 : 64;
                    memcpy(pair + pl, sig, 64); pl += 64;
                    if (ht) pair[pl++] = ht;
                }
                bs_wipe(&kp, sizeof(kp));
            }
            bs_wipe(priv, 32); bs_wipe(chain, 32);
            if (rc == BS_OK && emit(out, cap, &n, pair, pl)) return BS_ERR_BUFFER;
        }
        uint8_t z = 0;
        if (emit(out, cap, &n, &z, 1)) return BS_ERR_BUFFER;
    }
    if (rc != BS_OK) return rc;
    size_t rest = p->n_out ? p->out[0].map_start : p->in[p->n_in - 1].map_end + 1;
    if (emit(out, cap, &n, p->buf + rest, p->len - rest)) return BS_ERR_BUFFER;
    *outlen = n;
    return BS_OK;
}
