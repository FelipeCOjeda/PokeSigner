// Screens and button handling. Works with or without the display; without it
// every screen is a no-op and the serial console carries the information.
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
#if CONFIG_PS_DISPLAY_ST7789
#define SCREEN_H CONFIG_PS_LCD_HEIGHT
#else
#define SCREEN_H 320
#endif

static int s_disp;
static char s_name[24];
static uint32_t s_fp;
static ui_link_t s_link = UI_ADV;
static volatile int s_modal;

/* ------------------------------------------------------------ button */

static int pressed(void) { return gpio_get_level(BTN) == 0; }

int ui_wait_button(int hold_ms, int timeout_ms, int (*abort_cb)(void)) {
    int64_t t0 = esp_timer_get_time();
    while (pressed()) vTaskDelay(pdMS_TO_TICKS(10));  // must start released
    for (;;) {
        if (abort_cb && abort_cb()) return BTN_TIMEOUT;
        if (timeout_ms > 0 && (esp_timer_get_time() - t0) / 1000 > timeout_ms) return BTN_TIMEOUT;
        if (pressed()) {
            int64_t p0 = esp_timer_get_time();
            while (pressed()) vTaskDelay(pdMS_TO_TICKS(10));
            int held = (int)((esp_timer_get_time() - p0) / 1000);
            if (held < 40) continue;  // debounce
            return held >= hold_ms ? BTN_LONG : BTN_SHORT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ui_init(void) {
    gpio_config_t io = {.pin_bit_mask = 1ULL << BTN, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io);
    s_disp = disp_init() == 0;
    if (s_disp) {
        disp_line_t l[] = {{"", C_WHITE, C_BLACK, 3, 0}, {"Poke", C_YELLOW, C_BLACK, 3, 1},
                           {"Signer", C_YELLOW, C_BLACK, 3, 1}, {"", C_WHITE, C_BLACK, 1, 0},
                           {"iniciando...", C_GRAY, C_BLACK, 1, 1}};
        disp_page(l, sizeof(l) / sizeof(l[0]));
    }
}

int ui_has_display(void) { return s_disp; }

/* ------------------------------------------------------------ simple screens */

#define L(t, fg, sc, ctr) ((disp_line_t){(t), (fg), C_BLACK, (sc), (ctr)})

void ui_set_identity(const char *name, uint32_t fp) {
    strncpy(s_name, name, sizeof(s_name) - 1); s_fp = fp;
}

void ui_idle(ui_link_t link) {
    s_link = link;
    if (!s_disp || s_modal) return;
    char fp[16];
    snprintf(fp, sizeof(fp), "%08" PRIx32, s_fp);
    const char *st = link == UI_SECURE ? "conectado (seguro)" : link == UI_CONNECTED ? "conectado..." : "aguardando conexao";
    uint16_t sc = link == UI_SECURE ? C_GREEN : link == UI_CONNECTED ? C_YELLOW : C_GRAY;
    disp_line_t l[] = {
        L("PokeSigner", C_YELLOW, 2, 1), L("", C_WHITE, 1, 0),
        L("Fingerprint", C_GRAY, 1, 1), L(fp, C_WHITE, 2, 1), L("", C_WHITE, 1, 0),
        L("Bluetooth", C_GRAY, 1, 1), L(s_name, C_CYAN, 1, 1), L(st, sc, 1, 1),
    };
    disp_page(l, sizeof(l) / sizeof(l[0]));
}

void ui_refresh_idle(void) { ui_idle(s_link); }

void ui_passkey(uint32_t code) {
    if (!s_disp) return;
    char c[8];
    snprintf(c, sizeof(c), "%06" PRIu32, code);
    disp_line_t l[] = {
        L("PAREAMENTO", C_YELLOW, 2, 1), L("", C_WHITE, 1, 0),
        L("Digite no celular:", C_WHITE, 1, 1), L("", C_WHITE, 1, 0),
        L(c, C_GREEN, 3, 1), L("", C_WHITE, 1, 0),
        L("Se voce nao iniciou", C_GRAY, 1, 1), L("um pareamento,", C_GRAY, 1, 1), L("ignore.", C_GRAY, 1, 1),
    };
    disp_page(l, sizeof(l) / sizeof(l[0]));
}

void ui_msg(const char *title, const char *l1, const char *l2, uint16_t color) {
    if (!s_disp) return;
    disp_line_t l[] = {L("", C_WHITE, 2, 0), L(title, color, 2, 1), L("", C_WHITE, 1, 0),
                       L(l1 ? l1 : "", C_WHITE, 1, 1), L(l2 ? l2 : "", C_WHITE, 1, 1)};
    disp_page(l, sizeof(l) / sizeof(l[0]));
}

void ui_seed_words(const char *mnemonic) {
    if (!s_disp) return;
    char words[24][12], txt[24][24];
    int n = 0;
    const char *p = mnemonic;
    while (*p && n < 24) {
        int k = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && k < 11) words[n][k++] = *p++;
        words[n][k] = 0;
        if (k) n++;
    }
    int pages = (n + 11) / 12;
    for (int pg = 0; pg < pages; pg++) {
        disp_line_t l[17];
        size_t m = 0;
        char title[16];
        snprintf(title, sizeof(title), "SEED %d/%d", pg + 1, pages);
        l[m++] = L(title, C_YELLOW, 2, 1);
        for (int i = pg * 12; i < n && i < pg * 12 + 12; i++) {
            snprintf(txt[i], sizeof(txt[i]), " %2d. %.11s", i + 1, words[i]);
            l[m++] = L(txt[i], C_WHITE, 1, 0);
        }
        l[m++] = L("", C_WHITE, 1, 0);
        l[m++] = L("Anote em papel.", C_ORANGE, 1, 1);
        l[m++] = L(pg + 1 < pages ? "BOOT: proxima >" : "BOOT: concluir", C_GRAY, 1, 1);
        disp_page(l, m);
        ui_wait_button(100000, 0, NULL);
    }
    memset(words, 0, sizeof(words)); memset(txt, 0, sizeof(txt));
    ui_msg("Confirme", "Digite no serial as", "palavras pedidas.", C_YELLOW);
}

/* ------------------------------------------------------------ review */

#define MAX_LINES 20
#define MAX_PAGES (BS_MAX_OUTPUTS + 4)

typedef struct { disp_line_t l[MAX_LINES]; size_t n; } page_t;

static page_t s_pages[1];         // built one at a time
static char s_txt[MAX_LINES][24];
static size_t s_ti;

static const char *keep(const char *src) {
    if (s_ti >= MAX_LINES) return "";
    strncpy(s_txt[s_ti], src, sizeof(s_txt[0]) - 1);
    s_txt[s_ti][sizeof(s_txt[0]) - 1] = 0;
    return s_txt[s_ti++];
}

static void add(page_t *pg, const char *t, uint16_t fg, int sc, int ctr) {
    if (pg->n < MAX_LINES) pg->l[pg->n++] = (disp_line_t){keep(t), fg, C_BLACK, (uint8_t)sc, (uint8_t)ctr};
}

static void add_amount(page_t *pg, uint64_t sats, uint16_t fg) {
    char a[24];
    snprintf(a, sizeof(a), "%" PRIu64 ".%08" PRIu64, (uint64_t)(sats / 100000000u), (uint64_t)(sats % 100000000u));
    add(pg, a, fg, strlen(a) <= 10 ? 2 : 1, 1);
    add(pg, "BTC", fg, 1, 1);
}

static size_t count_dest(const bs_psbt_t *p) {
    size_t k = 0;
    for (size_t i = 0; i < p->n_out; i++) if (!p->out[i].is_change) k++;
    return k;
}

static int fee_high(const bs_psbt_t *p) {
    uint64_t sent = p->out_total - p->change_total;
    return p->fee_known && (p->fee > 1000000ULL || (sent && p->fee * 10 > sent));
}

// Page order: 0 summary, 1..K destinations, [change], confirm.
static void build_page(const bs_psbt_t *p, int idx, int total, page_t *pg) {
    pg->n = 0; s_ti = 0;
    size_t K = count_dest(p);
    int has_change = p->change_total > 0;
    char b[64];
    if (idx == 0) {
        add(pg, "REVISAR TX", C_YELLOW, 2, 1);
        add(pg, p->testnet ? "TESTNET" : "MAINNET", p->testnet ? C_ORANGE : C_CYAN, 1, 1);
        add(pg, "", C_WHITE, 1, 0);
        snprintf(b, sizeof(b), "Entradas:  %u", (unsigned)p->n_in);
        add(pg, b, C_WHITE, 1, 0);
        snprintf(b, sizeof(b), "Assinando: %u", (unsigned)p->n_sign);
        add(pg, b, C_WHITE, 1, 0);
        snprintf(b, sizeof(b), "Destinos:  %u", (unsigned)K);
        add(pg, b, C_WHITE, 1, 0);
        add(pg, "", C_WHITE, 1, 0);
        add(pg, "Enviando:", C_GRAY, 1, 0);
        add_amount(pg, p->out_total - p->change_total, C_GREEN);
        add(pg, "Taxa:", C_GRAY, 1, 0);
        if (p->fee_known) add_amount(pg, p->fee, fee_high(p) ? C_RED : C_WHITE);
        else add(pg, "DESCONHECIDA", C_RED, 1, 1);
        if (fee_high(p)) add(pg, "!! TAXA ALTA !!", C_RED, 1, 1);
    } else if (idx <= (int)K) {
        size_t k = 0, i;
        for (i = 0; i < p->n_out; i++) if (!p->out[i].is_change && ++k == (size_t)idx) break;
        const bs_output_t *o = &p->out[i];
        snprintf(b, sizeof(b), "PARA %d/%u", idx, (unsigned)K);
        add(pg, b, C_YELLOW, 2, 1);
        add(pg, "", C_WHITE, 1, 0);
        char addr[100];
        if (bs_script_to_addr(o->spk, o->spk_len, p->testnet, addr, sizeof(addr)) == 0) {
            // groups of 4 chars, 4 groups per line
            size_t al = strlen(addr);
            for (size_t s = 0; s < al; s += 16) {
                char line[24]; size_t q = 0;
                for (size_t c = s; c < al && c < s + 16; c++) {
                    if (c > s && (c - s) % 4 == 0) line[q++] = ' ';
                    line[q++] = addr[c];
                }
                line[q] = 0;
                add(pg, line, C_WHITE, 1, 1);
            }
        } else if (o->spk_len && o->spk[0] == 0x6a) {
            add(pg, "OP_RETURN", C_ORANGE, 1, 1);
            snprintf(b, sizeof(b), "(%u bytes de dados)", (unsigned)o->spk_len);
            add(pg, b, C_GRAY, 1, 1);
        } else {
            add(pg, "SCRIPT NAO PADRAO", C_RED, 1, 1);
        }
        add(pg, "", C_WHITE, 1, 0);
        add(pg, "Valor:", C_GRAY, 1, 0);
        add_amount(pg, o->amount, C_GREEN);
    } else if (has_change && idx == (int)K + 1) {
        add(pg, "TROCO", C_YELLOW, 2, 1);
        add(pg, "", C_WHITE, 1, 0);
        add(pg, "Volta para esta seed", C_WHITE, 1, 1);
        add(pg, "(verificado)", C_GREEN, 1, 1);
        add(pg, "", C_WHITE, 1, 0);
        add_amount(pg, p->change_total, C_WHITE);
    } else {
        add(pg, "CONFIRMAR?", C_YELLOW, 2, 1);
        add(pg, "", C_WHITE, 1, 0);
        add(pg, "Enviar:", C_GRAY, 1, 0);
        add_amount(pg, p->out_total - p->change_total, C_GREEN);
        if (p->fee_known) {
            char a[32];
            snprintf(a, sizeof(a), "Taxa %" PRIu64 " sats", p->fee);
            add(pg, a, fee_high(p) ? C_RED : C_WHITE, 1, 1);
        }
        if (p->locktime) { snprintf(b, sizeof(b), "Locktime %" PRIu32, p->locktime); add(pg, b, C_ORANGE, 1, 1); }
        add(pg, "", C_WHITE, 1, 0);
        add(pg, "Segure BOOT 2s", C_GREEN, 1, 1);
        add(pg, "= APROVAR", C_GREEN, 2, 1);
        add(pg, "", C_WHITE, 1, 0);
        add(pg, "Toque rapido", C_RED, 1, 1);
        add(pg, "= REJEITAR", C_RED, 1, 1);
        return;
    }
    // footer pinned to the last text row
    int used = 0;
    for (size_t i = 0; i < pg->n; i++) used += 16 * pg->l[i].scale;
    while (used + 32 <= SCREEN_H && pg->n < MAX_LINES - 1) {
        add(pg, "", C_WHITE, 1, 0); used += 16;
    }
    snprintf(b, sizeof(b), "%d/%d  BOOT: proxima >", idx + 1, total);
    add(pg, b, C_GRAY, 1, 1);
}

int ui_review(const bs_psbt_t *p, int timeout_ms, int (*abort_cb)(void)) {
    if (!s_disp) {
        // No screen: details are on the serial console; hold = approve, tap = reject.
        int r = ui_wait_button(2000, timeout_ms, abort_cb);
        return r == BTN_LONG ? 1 : r == BTN_SHORT ? 0 : -1;
    }
    s_modal = 1;
    int total = 1 + (int)count_dest(p) + (p->change_total > 0) + 1;
    int64_t t0 = esp_timer_get_time();
    int idx = 0, result = -1;
    for (;;) {
        build_page(p, idx, total, &s_pages[0]);
        // the line height budget can overflow 320px on dense pages; disp_page clips
        disp_page(s_pages[0].l, s_pages[0].n);
        int left = timeout_ms - (int)((esp_timer_get_time() - t0) / 1000);
        if (left <= 0) break;
        int r = ui_wait_button(2000, left, abort_cb);
        if (r == BTN_TIMEOUT) break;
        if (idx < total - 1) { idx++; continue; }  // any press advances until the last page
        result = (r == BTN_LONG) ? 1 : 0;
        break;
    }
    if (result == 1) ui_msg("APROVADO", "assinando...", NULL, C_GREEN);
    else if (result == 0) ui_msg("REJEITADO", NULL, NULL, C_RED);
    else ui_msg("CANCELADO", "tempo esgotado ou", "desconectado", C_ORANGE);
    vTaskDelay(pdMS_TO_TICKS(1200));
    s_modal = 0;
    return result;
}
