// Screens in the style of 8-bit handheld RPGs: bordered text boxes, pixel font,
// arrow cursor and "more" prompt. Works without a display (all screens no-op).
#include "ui.h"
#include "display.h"
#include "bs_encode.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#define BTN CONFIG_PS_BUTTON_GPIO

static int s_disp;
static char s_name[24];
static uint32_t s_fp;
static ui_link_t s_link = UI_ADV;
static volatile int s_modal;
static grid_t G;

/* ------------------------------------------------------------ button */

static int pressed(void) { return gpio_get_level(BTN) == 0; }

int ui_wait_button(int hold_ms, int timeout_ms, int (*abort_cb)(void)) {
    int64_t t0 = esp_timer_get_time();
    while (pressed()) vTaskDelay(pdMS_TO_TICKS(10));
    for (;;) {
        if (abort_cb && abort_cb()) return BTN_TIMEOUT;
        if (timeout_ms > 0 && (esp_timer_get_time() - t0) / 1000 > timeout_ms) return BTN_TIMEOUT;
        if (pressed()) {
            int64_t p0 = esp_timer_get_time();
            while (pressed()) vTaskDelay(pdMS_TO_TICKS(10));
            int held = (int)((esp_timer_get_time() - p0) / 1000);
            if (held < 40) continue;
            return held >= hold_ms ? BTN_LONG : BTN_SHORT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ------------------------------------------------------------ grid helpers */

static void g_clear(void) {
    memset(&G, 0, sizeof(G));
    for (int r = 0; r < GH; r++)
        for (int c = 0; c < GW; c++) { G.ch[r][c] = T_DOTS[0]; G.fg[r][c] = P_PURPLE; G.bg[r][c] = P_LAV; }
}

static void g_box(int r0, int r1) {
    for (int r = r0; r <= r1; r++)
        for (int c = 0; c < GW; c++) {
            char t = ' ';
            if (r == r0) t = c == 0 ? T_TL[0] : c == GW - 1 ? T_TR[0] : T_HT[0];
            else if (r == r1) t = c == 0 ? T_BL[0] : c == GW - 1 ? T_BR[0] : T_HB[0];
            else if (c == 0) t = T_VL[0];
            else if (c == GW - 1) t = T_VR[0];
            G.ch[r][c] = t; G.fg[r][c] = P_INK; G.bg[r][c] = P_WHITE;
        }
}

// text inside a box: columns 1..18
static void g_text(int r, int c, const char *s, int fg) {
    for (; *s && c < GW - 1; s++, c++) { G.ch[r][c] = *s; G.fg[r][c] = (uint8_t)fg; }
}
static void g_center(int r, const char *s, int fg) {
    int n = (int)strlen(s); if (n > GW - 2) n = GW - 2;
    g_text(r, 1 + (GW - 2 - n) / 2, s, fg);
}
static void g_right(int r, const char *s, int fg) {
    int n = (int)strlen(s); if (n > GW - 2) n = GW - 2;
    g_text(r, GW - 1 - n, s, fg);
}
static void g_big(int r, const char *s, int fg) {  // uses rows r and r+1
    G.big[r] = 1;
    g_text(r, 1, s, fg);
}
static void g_show(void) { if (s_disp) disp_grid(&G); }

// word-wraps text into rows [r0, r1] of a box
static void g_wrap(int r0, int r1, const char *s, int fg) {
    int r = r0;
    while (*s && r <= r1) {
        while (*s == ' ') s++;
        int n = (int)strlen(s), take = n > 18 ? 18 : n;
        if (n > 18) { int k = take; while (k > 0 && s[k] != ' ') k--; if (k > 0) take = k; }
        char line[20]; memcpy(line, s, take); line[take] = 0;
        g_text(r++, 1, line, fg);
        s += take;
    }
}

static void fmt_btc(uint64_t sats, char *out, size_t n) {
    snprintf(out, n, "%" PRIu64 ".%08" PRIu64, (uint64_t)(sats / 100000000u), (uint64_t)(sats % 100000000u));
}

/* ------------------------------------------------------------ screens */

void ui_init(uint8_t orientation) {
    gpio_config_t io = {.pin_bit_mask = 1ULL << BTN, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    s_disp = disp_init(orientation) == 0;
    g_clear();
    g_box(2, 7);
    g_center(4, "POKESIGNER", P_PURPLE);
    g_center(5, "iniciando...", P_INK);
    g_show();
}

int ui_has_display(void) { return s_disp; }

void ui_rotate(uint8_t orientation) {
    disp_set_orientation(orientation);
    ui_refresh_idle();
}

void ui_set_identity(const char *name, uint32_t fp) {
    strncpy(s_name, name, sizeof(s_name) - 1); s_fp = fp;
}

void ui_idle(ui_link_t link) {
    s_link = link;
    if (!s_disp || s_modal) return;
    char b[24];
    g_clear();
    g_box(0, 9);
    g_text(1, 1, "POKESIGNER", P_PURPLE);
    g_right(1, "v0.3.3", P_LAV);
    snprintf(b, sizeof(b), "ID   %08" PRIX32, s_fp);
    g_text(3, 1, b, P_INK);
    const char *sfx = strchr(s_name, '-');
    snprintf(b, sizeof(b), "BLE  %s", sfx ? sfx + 1 : s_name);
    g_text(4, 1, b, P_INK);
    g_text(6, 1, T_ARROW, P_INK);
    if (link == UI_SECURE) g_text(6, 3, "CONECTADO", P_GREEN);
    else if (link == UI_CONNECTED) g_text(6, 3, "PAREANDO...", P_PURPLE);
    else g_text(6, 3, "AGUARDANDO", P_INK);
    g_text(8, 1, "Envie um PSBT", P_PURPLE);
    g_show();
}

void ui_refresh_idle(void) { ui_idle(s_link); }

void ui_passkey(uint32_t code) {
    if (!s_disp) return;
    char c[8];
    snprintf(c, sizeof(c), "%06" PRIu32, code);
    g_clear();
    g_box(0, 9);
    g_text(1, 1, "PAREAMENTO", P_PURPLE);
    g_text(3, 1, "Digite no celular:", P_INK);
    g_big(5, c, P_INK);
    g_text(8, 1, "Nao pediu? Ignore.", P_LAV);
    g_show();
}

void ui_msg(const char *title, const char *l1, const char *l2, uint16_t color) {
    if (!s_disp) return;
    int pal = color == C_RED ? P_RED : color == C_GREEN ? P_GREEN : color == C_ORANGE ? P_RED : P_PURPLE;
    g_clear();
    g_box(4, 9);
    g_text(5, 1, title, pal);
    char buf[80];
    snprintf(buf, sizeof(buf), "%s%s%s", l1 ? l1 : "", l1 && l2 ? " " : "", l2 ? l2 : "");
    g_wrap(6, 8, buf, P_INK);
    g_show();
}

void ui_seed_words(const char *mnemonic) {
    if (!s_disp) return;
    char words[24][12];
    int n = 0;
    const char *p = mnemonic;
    while (*p && n < 24) {
        int k = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && k < 11) words[n][k++] = *p++;
        words[n][k] = 0;
        if (k) n++;
    }
    int per = 6, pages = (n + per - 1) / per;
    for (int pg = 0; pg < pages; pg++) {
        char t[24];
        g_clear();
        g_box(0, 9);
        snprintf(t, sizeof(t), "SEED %d/%d", pg + 1, pages);
        g_text(1, 1, t, P_PURPLE);
        g_right(1, "ANOTE!", P_RED);
        for (int i = pg * per, row = 2; i < n && i < pg * per + per; i++, row++) {
            snprintf(t, sizeof(t), "%2d %s", i + 1, words[i]);
            g_text(row, 2, t, P_INK);
        }
        g_right(8, T_DOWN, P_INK);
        g_show();
        ui_wait_button(100000, 0, NULL);
    }
    memset(words, 0, sizeof(words));
    memset(&G, 0, sizeof(G));
    ui_msg("CONFIRME", "Digite no computador", "as palavras pedidas.", C_YELLOW);
}

/* ------------------------------------------------------------ review */

static size_t count_dest(const bs_psbt_t *p) {
    size_t k = 0;
    for (size_t i = 0; i < p->n_out; i++) if (!p->out[i].is_change) k++;
    return k;
}

static int fee_high(const bs_psbt_t *p) {
    uint64_t sent = p->out_total - p->change_total;
    return p->fee_known && (p->fee > 1000000ULL || (sent && p->fee * 10 > sent));
}

static void footer(int idx, int total) {
    char b[12];
    snprintf(b, sizeof(b), "%d/%d", idx + 1, total);
    g_text(8, 1, b, P_LAV);
    g_right(8, T_DOWN, P_INK);
}

static void amount_line(int r, uint64_t sats, int fg) {
    char a[24], line[24];
    fmt_btc(sats, a, sizeof(a));
    snprintf(line, sizeof(line), "%s " T_BTC, a);
    g_center(r, line, fg);
}

// Pages: 0 encounter/summary, 1..K destinations, [change], confirm.
static void build_page(const bs_psbt_t *p, int idx, int total) {
    size_t K = count_dest(p);
    int has_change = p->change_total > 0;
    char b[40];
    g_clear();
    if (idx == 0) {
        g_box(0, 4);
        g_text(1, 1, "PSBT", P_PURPLE);
        g_right(1, p->testnet ? "TESTNET" : "MAINNET", p->testnet ? P_RED : P_BLUE);
        snprintf(b, sizeof(b), "ASSINA %u DE %u", (unsigned)p->n_sign, (unsigned)p->n_in);
        g_text(2, 1, b, P_INK);
        if (p->fee_known) snprintf(b, sizeof(b), "TAXA %" PRIu64 " sat", p->fee);
        else snprintf(b, sizeof(b), "TAXA ???");
        g_text(3, 1, b, fee_high(p) || !p->fee_known ? P_RED : P_INK);
        g_box(5, 9);
        g_text(6, 1, "Um PSBT selvagem", P_INK);
        snprintf(b, sizeof(b), "apareceu! (%u dest)", (unsigned)K);
        g_text(7, 1, b, P_INK);
        footer(idx, total);
    } else if (idx <= (int)K) {
        size_t k = 0, i;
        for (i = 0; i < p->n_out; i++) if (!p->out[i].is_change && ++k == (size_t)idx) break;
        const bs_output_t *o = &p->out[i];
        g_box(0, 9);
        snprintf(b, sizeof(b), "PARA %d/%u", idx, (unsigned)K);
        g_text(1, 1, b, P_PURPLE);
        char addr[100];
        if (bs_script_to_addr(o->spk, o->spk_len, p->testnet, addr, sizeof(addr)) == 0) {
            // 16 chars per row, 4-char groups in alternating colors for easy checking
            size_t al = strlen(addr);
            for (size_t s = 0, row = 2; s < al && row <= 5; s += 16, row++)
                for (size_t c = s; c < al && c < s + 16; c++) {
                    int col = 2 + (int)(c - s);
                    G.ch[row][col] = addr[c];
                    G.fg[row][col] = ((c - s) / 4) % 2 ? P_PURPLE : P_INK;
                }
        } else if (o->spk_len && o->spk[0] == 0x6a) {
            g_text(3, 2, "OP_RETURN", P_RED);
            snprintf(b, sizeof(b), "%u bytes de dados", (unsigned)o->spk_len);
            g_text(4, 2, b, P_INK);
        } else {
            g_text(3, 2, "SCRIPT ESTRANHO!", P_RED);
        }
        g_text(6, 1, "VALOR", P_PURPLE);
        amount_line(7, o->amount, P_GREEN);
        footer(idx, total);
    } else if (has_change && idx == (int)K + 1) {
        g_box(0, 9);
        g_text(1, 1, "TROCO", P_PURPLE);
        g_text(3, 1, "Volta para voce:", P_INK);
        amount_line(4, p->change_total, P_GREEN);
        g_text(6, 1, "(verificado)", P_LAV);
        footer(idx, total);
    } else {
        g_box(0, 4);
        g_text(1, 1, "ENVIAR", P_PURPLE);
        amount_line(2, p->out_total - p->change_total, P_GREEN);
        if (p->fee_known) {
            snprintf(b, sizeof(b), "TAXA %" PRIu64 " sat%s", p->fee, fee_high(p) ? " !" : "");
            g_text(3, 1, b, fee_high(p) ? P_RED : P_INK);
        }
        g_box(5, 9);
        g_text(6, 1, T_ARROW "APROVAR: SEGURE", P_GREEN);
        g_text(7, 1, " REJEITAR: TOQUE", P_RED);
        if (p->locktime) { snprintf(b, sizeof(b), "locktime %" PRIu32, p->locktime); g_text(8, 1, b, P_PURPLE); }
    }
}

int ui_review(const bs_psbt_t *p, int timeout_ms, int (*abort_cb)(void)) {
    if (!s_disp) {
        int r = ui_wait_button(2000, timeout_ms, abort_cb);
        return r == BTN_LONG ? 1 : r == BTN_SHORT ? 0 : -1;
    }
    s_modal = 1;
    int total = 1 + (int)count_dest(p) + (p->change_total > 0) + 1;
    int64_t t0 = esp_timer_get_time();
    int idx = 0, result = -1;
    for (;;) {
        build_page(p, idx, total);
        g_show();
        int left = timeout_ms - (int)((esp_timer_get_time() - t0) / 1000);
        if (left <= 0) break;
        int r = ui_wait_button(2000, left, abort_cb);
        if (r == BTN_TIMEOUT) break;
        if (idx < total - 1) { idx++; continue; }  // every page must be seen
        result = (r == BTN_LONG) ? 1 : 0;
        break;
    }
    if (result == 1) ui_msg("APROVADO!", "Assinando o PSBT...", NULL, C_GREEN);
    else if (result == 0) ui_msg("REJEITADO", "Voce recusou o PSBT.", NULL, C_RED);
    else ui_msg("FUGIU!", "O PSBT fugiu! (tempo", "esgotado ou desconexao)", C_ORANGE);
    vTaskDelay(pdMS_TO_TICKS(1200));
    s_modal = 0;
    return result;
}
