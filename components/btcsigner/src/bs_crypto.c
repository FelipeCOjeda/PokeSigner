// Portable SHA-256, SHA-512, RIPEMD-160, HMAC-SHA512, PBKDF2-HMAC-SHA512.
#include "bs_crypto.h"
#include <string.h>

void bs_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* ---------------- SHA-256 ---------------- */
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

#define ROR32(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t s[8], const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i-15],7) ^ ROR32(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR32(w[i-2],17) ^ ROR32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s[0]; b=s[1]; c=s[2]; d=s[3]; e=s[4]; f=s[5]; g=s[6]; h=s[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + (ROR32(e,6) ^ ROR32(e,11) ^ ROR32(e,25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        t2 = (ROR32(a,2) ^ ROR32(a,13) ^ ROR32(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=h;
    bs_wipe(w, sizeof(w));
}

void bs_sha256_init(bs_sha256_ctx *c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->s, iv, sizeof(iv));
    c->len = 0;
}

void bs_sha256_update(bs_sha256_ctx *c, const void *data, size_t len) {
    const uint8_t *p = data;
    if (!len) return;
    size_t fill = c->len % 64;
    c->len += len;
    if (fill) {
        size_t n = 64 - fill < len ? 64 - fill : len;
        memcpy(c->buf + fill, p, n); p += n; len -= n;
        if (fill + n < 64) return;
        sha256_block(c->s, c->buf);
    }
    while (len >= 64) { sha256_block(c->s, p); p += 64; len -= 64; }
    if (len) memcpy(c->buf, p, len);
}

void bs_sha256_final(bs_sha256_ctx *c, uint8_t out[32]) {
    uint64_t bits = c->len * 8;
    uint8_t pad[72] = {0x80};
    size_t fill = c->len % 64;
    size_t padlen = (fill < 56) ? 56 - fill : 120 - fill;
    for (int i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (56 - 8*i));
    bs_sha256_update(c, pad, padlen + 8);
    for (int i = 0; i < 8; i++) {
        out[4*i] = c->s[i] >> 24; out[4*i+1] = c->s[i] >> 16;
        out[4*i+2] = c->s[i] >> 8; out[4*i+3] = c->s[i];
    }
    bs_wipe(c, sizeof(*c));
}

void bs_sha256(const void *data, size_t len, uint8_t out[32]) {
    bs_sha256_ctx c; bs_sha256_init(&c); bs_sha256_update(&c, data, len); bs_sha256_final(&c, out);
}

void bs_sha256d(const void *data, size_t len, uint8_t out[32]) {
    uint8_t t[32]; bs_sha256(data, len, t); bs_sha256(t, 32, out);
}

void bs_tagged_init(bs_sha256_ctx *c, const char *tag) {
    uint8_t th[32];
    bs_sha256(tag, strlen(tag), th);
    bs_sha256_init(c);
    bs_sha256_update(c, th, 32);
    bs_sha256_update(c, th, 32);
}

/* ---------------- SHA-512 ---------------- */
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL};

#define ROR64(x,n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(uint64_t s[8], const uint8_t *p) {
    uint64_t w[80], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++) {
        w[i] = 0;
        for (int j = 0; j < 8; j++) w[i] = (w[i] << 8) | p[8*i + j];
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i-15],1) ^ ROR64(w[i-15],8) ^ (w[i-15] >> 7);
        uint64_t s1 = ROR64(w[i-2],19) ^ ROR64(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s[0]; b=s[1]; c=s[2]; d=s[3]; e=s[4]; f=s[5]; g=s[6]; h=s[7];
    for (int i = 0; i < 80; i++) {
        t1 = h + (ROR64(e,14) ^ ROR64(e,18) ^ ROR64(e,41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        t2 = (ROR64(a,28) ^ ROR64(a,34) ^ ROR64(a,39)) + ((a & b) ^ (a & c) ^ (b & c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d; s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=h;
    bs_wipe(w, sizeof(w));
}

void bs_sha512_init(bs_sha512_ctx *c) {
    static const uint64_t iv[8] = {
        0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
    memcpy(c->s, iv, sizeof(iv));
    c->len = 0;
}

void bs_sha512_update(bs_sha512_ctx *c, const void *data, size_t len) {
    const uint8_t *p = data;
    if (!len) return;
    size_t fill = c->len % 128;
    c->len += len;
    if (fill) {
        size_t n = 128 - fill < len ? 128 - fill : len;
        memcpy(c->buf + fill, p, n); p += n; len -= n;
        if (fill + n < 128) return;
        sha512_block(c->s, c->buf);
    }
    while (len >= 128) { sha512_block(c->s, p); p += 128; len -= 128; }
    if (len) memcpy(c->buf, p, len);
}

void bs_sha512_final(bs_sha512_ctx *c, uint8_t out[64]) {
    uint64_t bits = c->len * 8;
    uint8_t pad[144] = {0x80};
    size_t fill = c->len % 128;
    size_t padlen = (fill < 112) ? 112 - fill : 240 - fill;
    // 128-bit length; upper 64 bits are zero
    for (int i = 0; i < 8; i++) pad[padlen + 8 + i] = (uint8_t)(bits >> (56 - 8*i));
    bs_sha512_update(c, pad, padlen + 16);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++) out[8*i + j] = (uint8_t)(c->s[i] >> (56 - 8*j));
    bs_wipe(c, sizeof(*c));
}

/* ---------------- RIPEMD-160 ---------------- */
#define ROL32(x,n) (((x) << (n)) | ((x) >> (32 - (n))))

static uint32_t rmd_f(int j, uint32_t x, uint32_t y, uint32_t z) {
    if (j < 16) return x ^ y ^ z;
    if (j < 32) return (x & y) | (~x & z);
    if (j < 48) return (x | ~y) ^ z;
    if (j < 64) return (x & z) | (y & ~z);
    return x ^ (y | ~z);
}

static const uint8_t RL[80] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15, 7,4,13,1,10,6,15,3,12,0,9,5,2,14,11,8,
    3,10,14,4,9,15,8,1,2,7,0,6,13,11,5,12, 1,9,11,10,0,8,12,4,13,3,7,15,14,5,6,2,
    4,0,5,9,7,12,2,10,14,1,3,8,11,6,15,13};
static const uint8_t RR[80] = {
    5,14,7,0,9,2,11,4,13,6,15,8,1,10,3,12, 6,11,3,7,0,13,5,10,14,15,8,12,4,9,1,2,
    15,5,1,3,7,14,6,9,11,8,12,2,10,0,4,13, 8,6,4,1,3,11,15,0,5,12,2,13,9,7,10,14,
    12,15,10,4,1,5,8,7,6,2,13,14,0,3,9,11};
static const uint8_t SL[80] = {
    11,14,15,12,5,8,7,9,11,13,14,15,6,7,9,8, 7,6,8,13,11,9,7,15,7,12,15,9,11,7,13,12,
    11,13,6,7,14,9,13,15,14,8,13,6,5,12,7,5, 11,12,14,15,14,15,9,8,9,14,5,6,8,6,5,12,
    9,15,5,11,6,8,13,12,5,12,13,14,11,8,5,6};
static const uint8_t SR[80] = {
    8,9,9,11,13,15,15,5,7,7,8,11,14,14,12,6, 9,13,15,7,12,8,9,11,7,7,12,7,6,15,13,11,
    9,7,15,11,8,6,6,14,12,13,5,14,13,13,7,5, 15,5,8,11,14,14,6,14,6,9,12,9,12,5,15,8,
    8,5,12,9,12,5,14,6,8,13,6,5,15,13,11,11};
static const uint32_t KL[5] = {0x00000000,0x5a827999,0x6ed9eba1,0x8f1bbcdc,0xa953fd4e};
static const uint32_t KR[5] = {0x50a28be6,0x5c4dd124,0x6d703ef3,0x7a6d76e9,0x00000000};

static void rmd_block(uint32_t h[5], const uint8_t *p) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = p[4*i] | (uint32_t)p[4*i+1] << 8 | (uint32_t)p[4*i+2] << 16 | (uint32_t)p[4*i+3] << 24;
    uint32_t al=h[0], bl=h[1], cl=h[2], dl=h[3], el=h[4];
    uint32_t ar=h[0], br=h[1], cr=h[2], dr=h[3], er=h[4], t;
    for (int j = 0; j < 80; j++) {
        t = ROL32(al + rmd_f(j, bl, cl, dl) + x[RL[j]] + KL[j/16], SL[j]) + el;
        al = el; el = dl; dl = ROL32(cl, 10); cl = bl; bl = t;
        t = ROL32(ar + rmd_f(79 - j, br, cr, dr) + x[RR[j]] + KR[j/16], SR[j]) + er;
        ar = er; er = dr; dr = ROL32(cr, 10); cr = br; br = t;
    }
    t = h[1] + cl + dr; h[1] = h[2] + dl + er; h[2] = h[3] + el + ar;
    h[3] = h[4] + al + br; h[4] = h[0] + bl + cr; h[0] = t;
}

void bs_ripemd160(const void *data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    const uint8_t *p = data;
    size_t rem = len;
    while (rem >= 64) { rmd_block(h, p); p += 64; rem -= 64; }
    uint8_t buf[128] = {0};
    memcpy(buf, p, rem);
    buf[rem] = 0x80;
    size_t total = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[total - 8 + i] = (uint8_t)(bits >> (8*i));
    rmd_block(h, buf);
    if (total == 128) rmd_block(h, buf + 64);
    for (int i = 0; i < 5; i++) {
        out[4*i] = h[i]; out[4*i+1] = h[i] >> 8; out[4*i+2] = h[i] >> 16; out[4*i+3] = h[i] >> 24;
    }
}

void bs_hash160(const void *data, size_t len, uint8_t out[20]) {
    uint8_t t[32]; bs_sha256(data, len, t); bs_ripemd160(t, 32, out);
}

/* ---------------- HMAC / PBKDF2 ---------------- */
typedef struct { bs_sha512_ctx inner, outer; } hmac512_ctx;

static void hmac512_init(hmac512_ctx *h, const uint8_t *key, size_t klen) {
    uint8_t k[128] = {0}, pad[128];
    if (klen > 128) {
        bs_sha512_ctx c; bs_sha512_init(&c); bs_sha512_update(&c, key, klen); bs_sha512_final(&c, k);
    } else {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < 128; i++) pad[i] = k[i] ^ 0x36;
    bs_sha512_init(&h->inner); bs_sha512_update(&h->inner, pad, 128);
    for (int i = 0; i < 128; i++) pad[i] = k[i] ^ 0x5c;
    bs_sha512_init(&h->outer); bs_sha512_update(&h->outer, pad, 128);
    bs_wipe(k, sizeof(k)); bs_wipe(pad, sizeof(pad));
}

static void hmac512_final(hmac512_ctx *h, uint8_t out[64]) {
    uint8_t t[64];
    bs_sha512_final(&h->inner, t);
    bs_sha512_update(&h->outer, t, 64);
    bs_sha512_final(&h->outer, out);
    bs_wipe(t, sizeof(t));
}

void bs_hmac_sha512(const uint8_t *key, size_t klen, const uint8_t *msg, size_t mlen, uint8_t out[64]) {
    hmac512_ctx h;
    hmac512_init(&h, key, klen);
    bs_sha512_update(&h.inner, msg, mlen);
    hmac512_final(&h, out);
}

void bs_pbkdf2_hmac_sha512(const uint8_t *pass, size_t plen, const uint8_t *salt, size_t slen,
                           uint32_t iters, uint8_t out[64]) {
    // Single block (dkLen = 64). Precompute keyed state once for speed.
    hmac512_ctx base, h;
    uint8_t u[64], t[64];
    static const uint8_t one[4] = {0, 0, 0, 1};
    hmac512_init(&base, pass, plen);
    h = base;
    bs_sha512_update(&h.inner, salt, slen);
    bs_sha512_update(&h.inner, one, 4);
    hmac512_final(&h, u);
    memcpy(t, u, 64);
    for (uint32_t i = 1; i < iters; i++) {
        h = base;
        bs_sha512_update(&h.inner, u, 64);
        hmac512_final(&h, u);
        for (int j = 0; j < 64; j++) t[j] ^= u[j];
    }
    memcpy(out, t, 64);
    bs_wipe(&base, sizeof(base)); bs_wipe(&h, sizeof(h));
    bs_wipe(u, sizeof(u)); bs_wipe(t, sizeof(t));
}
