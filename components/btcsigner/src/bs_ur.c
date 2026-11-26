// UR "crypto-psbt" encoder: CBOR + CRC32 + Bytewords (minimal, uppercase).
#include "bs_ur.h"
#include <string.h>
#include <stdio.h>

// Bytewords: 256 four-letter words; the minimal form uses first + last letter.
static const char BW[] =
    "ableacidalsoapexaquaarchatomauntawayaxisbackbaldbarnbeltbetabiasbluebodybragbrewbulbbuzz"
    "calmcashcatschefcityclawcodecolacookcostcruxcurlcuspcyandarkdatadaysdelidicedietdoordown"
    "drawdropdrumdulldutyeacheasyechoedgeepicevenexamexiteyesfactfairfernfigsfilmfishfizzflap"
    "flewfluxfoxyfreefrogfuelfundgalagamegeargemsgiftgirlglowgoodgraygrimgurugushgyrohalfhang"
    "hardhawkheathelphighhillholyhopehornhutsicedideaidleinchinkyintoirisironitemjadejazzjoin"
    "joltjowljudojugsjumpjunkjurykeepkenokeptkeyskickkilnkingkitekiwiknoblamblavalazyleaflegs"
    "liarlimplionlistlogoloudloveluaulucklungmainmanymathmazememomenumeowmildmintmissmonknail"
    "navyneednewsnextnoonnotenumbobeyoboeomitonyxopenovalowlspaidpartpeckplaypluspoempoolpose"
    "puffpumapurrquadquizraceramprealredorichroadrockroofrubyruinrunsrustsafesagascarsetssilk"
    "skewslotsoapsolosongstubsurfswantacotasktaxitenttiedtimetinytoiltombtoystriptunatwinugly"
    "undouniturgeuservastveryvetovialvibeviewvisavoidvowswallwandwarmwaspwavewaxywebswhatwhen"
    "whizwolfworkyankyawnyellyogayurtzapszerozestzinczonezoom";

uint32_t bs_crc32(uint32_t crc, const uint8_t *d, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *d++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static size_t cbor_head(uint8_t *o, uint8_t major, uint64_t v) {   // canonical
    major <<= 5;
    if (v < 24) { o[0] = major | (uint8_t)v; return 1; }
    if (v <= 0xFF) { o[0] = major | 24; o[1] = (uint8_t)v; return 2; }
    if (v <= 0xFFFF) { o[0] = major | 25; o[1] = (uint8_t)(v >> 8); o[2] = (uint8_t)v; return 3; }
    o[0] = major | 26; o[1] = (uint8_t)(v >> 24); o[2] = (uint8_t)(v >> 16); o[3] = (uint8_t)(v >> 8); o[4] = (uint8_t)v;
    return 5;
}

static uint8_t msg_byte(const bs_ur_t *u, size_t i) {
    if (i < u->hdr_len) return u->hdr[i];
    i -= u->hdr_len;
    return i < u->psbt_len ? u->psbt[i] : 0;   // zero padding after the end
}

void bs_ur_init(bs_ur_t *u, const uint8_t *psbt, size_t len, size_t max_frag) {
    memset(u, 0, sizeof(*u));
    u->psbt = psbt; u->psbt_len = len;
    u->hdr_len = cbor_head(u->hdr, 2, len);           // major type 2 = byte string
    u->msg_len = u->hdr_len + len;
    u->crc = bs_crc32(bs_crc32(0, u->hdr, u->hdr_len), psbt, len);
    if (max_frag < 10) max_frag = 10;
    size_t count = (u->msg_len + max_frag - 1) / max_frag;
    u->frag_len = (u->msg_len + count - 1) / count;    // same as the reference encoder
    u->seq_len = (uint32_t)((u->msg_len + u->frag_len - 1) / u->frag_len);
}

typedef struct { char *o; size_t n, cap; uint32_t crc; int ok; } bw_t;

static void put_c(bw_t *w, char c) { if (w->n + 1 < w->cap) w->o[w->n++] = c; else w->ok = 0; }
static void put_s(bw_t *w, const char *s) { while (*s) put_c(w, *s++); }
static void put_b(bw_t *w, uint8_t b) {           // one byte as a minimal byteword
    w->crc = bs_crc32(w->crc, &b, 1);
    put_c(w, (char)(BW[b * 4] - 32));
    put_c(w, (char)(BW[b * 4 + 3] - 32));
}
static void put_crc(bw_t *w) {
    uint32_t c = w->crc;
    for (int k = 3; k >= 0; k--) { uint8_t b = (uint8_t)(c >> (8 * k)); put_c(w, (char)(BW[b * 4] - 32)); put_c(w, (char)(BW[b * 4 + 3] - 32)); }
}

size_t bs_ur_part(const bs_ur_t *u, uint32_t seq, char *out, size_t cap) {
    bw_t w = {out, 0, cap, 0, 1};
    put_s(&w, "UR:CRYPTO-PSBT/");
    if (u->seq_len <= 1) {
        for (size_t i = 0; i < u->msg_len; i++) put_b(&w, msg_byte(u, i));
    } else {
        seq = (seq - 1) % u->seq_len + 1;
        char t[24];
        snprintf(t, sizeof(t), "%u-%u/", (unsigned)seq, (unsigned)u->seq_len);
        put_s(&w, t);
        uint8_t h[24];
        size_t k = 0;
        h[k++] = 0x85;                                  // array(5)
        k += cbor_head(h + k, 0, seq);
        k += cbor_head(h + k, 0, u->seq_len);
        k += cbor_head(h + k, 0, u->msg_len);
        k += cbor_head(h + k, 0, u->crc);
        k += cbor_head(h + k, 2, u->frag_len);
        for (size_t i = 0; i < k; i++) put_b(&w, h[i]);
        size_t off = (size_t)(seq - 1) * u->frag_len;
        for (size_t i = 0; i < u->frag_len; i++) put_b(&w, msg_byte(u, off + i));
    }
    put_crc(&w);
    if (!w.ok) return 0;
    out[w.n] = 0;
    return w.n;
}
