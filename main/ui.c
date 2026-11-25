// Screens in the style of 8-bit handheld RPGs: bordered text boxes, pixel font,
// arrow cursor and "more" prompt. A small layout engine stacks boxes and wraps
// text so the same screens fit the C6 (20x10 tiles) and the C3 (16x16 tiles).
// Works without a display (all screens no-op).
#include "ui.h"
#include "display.h"
#include "bs_encode.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "ble.h"
#include "freertos/semphr.h"
#include "mascot.h"

#define BTN     CONFIG_PS_BUTTON_GPIO
#define BTN_REJ CONFIG_PS_REJECT_GPIO
#define BTN_NXT CONFIG_PS_NEXT_GPIO

static int s_disp;
static char s_name[24];
static uint32_t s_fp;
static ui_link_t s_link = UI_ADV;
static volatile int s_modal;
static grid_t G;
static SemaphoreHandle_t s_ui;          // serializes whole-screen drawing
static volatile int s_idle_visible;     // idle screen (with mascot) is on screen
#define UI_LOCK()   do { if (s_ui) xSemaphoreTake(s_ui, portMAX_DELAY); } while (0)
#define UI_UNLOCK() do { if (s_ui) xSemaphoreGive(s_ui); } while (0)

typedef struct { uint8_t cmd; char code[8]; } remote_ev_t;
static QueueHandle_t s_remote;
static volatile int s_reviewing;
static void (*s_status_cb)(const char *);

void ui_set_status_cb(void (*cb)(const char *)) { s_status_cb = cb; }
static void status(const char *m) { if (s_status_cb) s_status_cb(m); }

void ui_remote_event(uint8_t cmd, const uint8_t *payload, size_t len) {
    if (!s_remote) return;
    if (!s_reviewing) { status("nada para confirmar agora"); return; }
    remote_ev_t ev = {.cmd = cmd};
    size_t n = len < sizeof(ev.code) - 1 ? len : sizeof(ev.code) - 1;
    memcpy(ev.code, payload, n);
    xQueueSend(s_remote, &ev, 0);
}

/* ------------------------------------------------------------ buttons */

static uint64_t gpio_bit(int g) { return g >= 0 ? (1ULL << (g & 63)) : 0; }
static int lvl(int gpio) { return gpio >= 0 && gpio_get_level(gpio) == 0; }
// main button (BOOT) or the optional "next" button behave the same
static int pressed_main(void) { return lvl(BTN) || lvl(BTN_NXT); }
static int pressed_rej(void) { return lvl(BTN_REJ); }

// Waits for input. BTN_LONG if the main button is held >= hold_ms, BTN_SHORT for a
// tap, BTN_REJECT for the reject button, BTN_REMOTE for a phone event (ev filled).
static int wait_input(int hold_ms, int timeout_ms, int (*abort_cb)(void), remote_ev_t *ev) {
    int64_t t0 = esp_timer_get_time();
    while (pressed_main() || pressed_rej()) vTaskDelay(pdMS_TO_TICKS(10));
    for (;;) {
        if (abort_cb && abort_cb()) return BTN_TIMEOUT;
        if (timeout_ms > 0 && (esp_timer_get_time() - t0) / 1000 > timeout_ms) return BTN_TIMEOUT;
        if (ev && s_remote && xQueueReceive(s_remote, ev, 0) == pdTRUE) return BTN_REMOTE;
        if (pressed_rej()) {
            while (pressed_rej()) vTaskDelay(pdMS_TO_TICKS(10));
            return BTN_REJECT;
        }
        if (pressed_main()) {
            int64_t p0 = esp_timer_get_time();
            while (pressed_main()) vTaskDelay(pdMS_TO_TICKS(10));
            int held = (int)((esp_timer_get_time() - p0) / 1000);
            if (held < 40) continue;
            return held >= hold_ms ? BTN_LONG : BTN_SHORT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

int ui_wait_button(int hold_ms, int timeout_ms, int (*abort_cb)(void)) {
    int r = wait_input(hold_ms, timeout_ms, abort_cb, NULL);
    return r == BTN_REJECT ? BTN_SHORT : r;
}

/* ------------------------------------------------------------ grid primitives */

static void g_clear(void) {
    memset(&G, 0, sizeof(G));
    for (int r = 0; r < GH; r++)
        for (int c = 0; c < GW; c++) { G.ch[r][c] = T_DOTS[0]; G.fg[r][c] = P_PURPLE; G.bg[r][c] = P_LAV; }
}

static void g_box(int r0, int r1) {
    if (r1 >= GH) r1 = GH - 1;
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

static void g_put(int r, int c, const char *s, int n, int fg) {
    if (r < 0 || r >= GH) return;
    for (int i = 0; i < n && s[i] && c < GW - 1; i++, c++) { G.ch[r][c] = s[i]; G.fg[r][c] = (uint8_t)fg; }
}
static void g_show(void) { if (s_disp) disp_grid(&G); }

/* ------------------------------------------------------------ layout engine */

enum { L_TEXT, L_CENTER, L_PAIR, L_BIG, L_ADDR, L_AMOUNT, L_BLANK };
typedef struct { uint8_t type, fg, fg2; char a[72]; char b[24]; } line_t;
typedef struct { line_t l[16]; int n; char foot_l[12], foot_r[4]; int fill; } box_t;
static box_t s_boxes[3];
static int s_nbox;

static void lay_begin(void) { memset(s_boxes, 0, sizeof(s_boxes)); s_nbox = 0; }
static box_t *lay_box(void) { return s_nbox < 3 ? &s_boxes[s_nbox++] : &s_boxes[2]; }
static line_t *lay_line(box_t *b, int type, int fg) {
    line_t *l = &b->l[b->n < 16 ? b->n++ : 15];
    l->type = (uint8_t)type; l->fg = (uint8_t)fg; l->fg2 = (uint8_t)fg;
    return l;
}
static void L_text(box_t *b, int fg, const char *s)   { snprintf(lay_line(b, L_TEXT, fg)->a, 72, "%s", s); }
static void L_center(box_t *b, int fg, const char *s) { snprintf(lay_line(b, L_CENTER, fg)->a, 72, "%s", s); }
static void L_blank(box_t *b) { lay_line(b, L_BLANK, P_INK); }
static void L_pair(box_t *b, int fg, const char *l, int fg2, const char *r) {
    line_t *x = lay_line(b, L_PAIR, fg); x->fg2 = (uint8_t)fg2;
    snprintf(x->a, 72, "%s", l); snprintf(x->b, 24, "%s", r);
}
static void L_big(box_t *b, int fg, const char *s) { snprintf(lay_line(b, L_BIG, fg)->a, 72, "%s", s); }
static void L_addr(box_t *b, const char *s) { snprintf(lay_line(b, L_ADDR, P_INK)->a, 72, "%s", s); }
static void L_amount(box_t *b, int fg, uint64_t sats) {
    snprintf(lay_line(b, L_AMOUNT, fg)->a, 72, "%" PRIu64 ".%08" PRIu64,
             (uint64_t)(sats / 100000000u), (uint64_t)(sats % 100000000u));
}

#define ADDR_PER_ROW ((TW / 4) * 4)

// Word wrap inside TW columns. Returns the number of rows; draws them when draw=1.
static int wrap(const char *s, int r, int center, int fg, int draw) {
    int rows = 0;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        int n = (int)strlen(s), take = n > TW ? TW : n;
        if (n > TW) { int k = take; while (k > 0 && s[k] != ' ') k--; if (k > 0) take = k; }
        int w = take; while (w > 0 && s[w - 1] == ' ') w--;
        if (draw) g_put(r + rows, center ? 1 + (TW - w) / 2 : 1, s, w, fg);
        rows++; s += take;
    }
    return rows ? rows : 1;
}

static int line_rows(const line_t *l) {
    switch (l->type) {
    case L_TEXT: case L_CENTER: return wrap(l->a, 0, 0, 0, 0);
    case L_BIG: return 2;
    case L_ADDR: { int n = (int)strlen(l->a); return (n + ADDR_PER_ROW - 1) / ADDR_PER_ROW; }
    case L_AMOUNT: return (int)strlen(l->a) + 2 <= TW ? 1 : 2;
    default: return 1;
    }
}

static void draw_line(const line_t *l, int r) {
    switch (l->type) {
    case L_TEXT: wrap(l->a, r, 0, l->fg, 1); break;
    case L_CENTER: wrap(l->a, r, 1, l->fg, 1); break;
    case L_PAIR: {
        int nb = (int)strlen(l->b);
        g_put(r, 1, l->a, TW - nb - (nb ? 1 : 0), l->fg);
        g_put(r, GW - 1 - nb, l->b, nb, l->fg2);
        break;
    }
    case L_BIG: G.big[r] = 1; g_put(r, 1, l->a, TW, l->fg); break;
    case L_ADDR: {   // 4-char groups in alternating colors, no spaces
        int n = (int)strlen(l->a);
        for (int s = 0, row = r; s < n; s += ADDR_PER_ROW, row++)
            for (int c = s; c < n && c < s + ADDR_PER_ROW; c++) {
                int col = 1 + (TW - ADDR_PER_ROW) / 2 + (c - s);
                if (row < GH) { G.ch[row][col] = l->a[c]; G.fg[row][col] = ((c - s) / 4) % 2 ? P_PURPLE : P_INK; }
            }
        break;
    }
    case L_AMOUNT: {
        char t[32]; int n = (int)strlen(l->a);
        if (n + 2 <= TW) { snprintf(t, sizeof(t), "%s " T_BTC, l->a); g_put(r, 1 + (TW - n - 2) / 2, t, n + 2, l->fg); }
        else { g_put(r, 1 + (TW - n) / 2, l->a, n, l->fg); g_put(r + 1, 1 + (TW - 1) / 2, T_BTC, 1, l->fg); }
        break;
    }
    default: break;
    }
}

static int box_inner(const box_t *b) {
    int inner = 0;
    for (int j = 0; j < b->n; j++) inner += line_rows(&b->l[j]);
    return inner + (b->foot_l[0] || b->foot_r[0] ? 1 : 0);
}

static void draw_box(const box_t *b, int r0, int r1) {
    g_box(r0, r1);
    int y = r0 + 1;
    for (int j = 0; j < b->n && y < r1; j++) { draw_line(&b->l[j], y); y += line_rows(&b->l[j]); }
    if ((b->foot_l[0] || b->foot_r[0]) && r1 - 1 > r0) {
        g_put(r1 - 1, 1, b->foot_l, (int)strlen(b->foot_l), P_LAV);
        g_put(r1 - 1, GW - 1 - (int)strlen(b->foot_r), b->foot_r, (int)strlen(b->foot_r), P_INK);
    }
}

// Stacks the boxes top to bottom; the last one (or one with fill/footer) stretches to the
// bottom. If the page is too tall, blank lines are dropped first.
static void lay_render(void) {
    s_idle_visible = 0;
    g_clear();
    int need = 0;
    for (int i = 0; i < s_nbox; i++) need += 2 + box_inner(&s_boxes[i]);
    for (int i = s_nbox - 1; i >= 0 && need > GH; i--)
        for (int j = s_boxes[i].n - 1; j >= 0 && need > GH; j--)
            if (s_boxes[i].l[j].type == L_BLANK) {
                memmove(&s_boxes[i].l[j], &s_boxes[i].l[j + 1], (s_boxes[i].n - j - 1) * sizeof(line_t));
                s_boxes[i].n--; need--;
            }
    int spare = GH - need, r = 0;
    for (int i = 0; i < s_nbox && r < GH; i++) {
        box_t *b = &s_boxes[i];
        int h = box_inner(b) + 2;
        int stretch = b->fill || b->foot_l[0] || b->foot_r[0] || i == s_nbox - 1;
        if (stretch && spare > 0) { h += spare; spare = 0; }
        draw_box(b, r, r + h - 1);
        r += h;
    }
    g_show();
}

/* ------------------------------------------------------------ screens */

static void idle_sprite(const char *const *f) {
    disp_sprite(f, IDLE_SPR_X, IDLE_SPR_Y, 16 * IDLE_SPR_SC, 16 * IDLE_SPR_SC, 0, 0, IDLE_SPR_SC, 0, P_WHITE);
}

#if MASCOT_ANIM
// Blinks the idle mascot every few seconds.
static void blink_task(void *arg) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000 + (esp_random() % 3000)));
        if (!s_disp || s_modal || !s_idle_visible) continue;
        UI_LOCK();
        if (s_idle_visible) idle_sprite(SIGNY_BLINK);
        UI_UNLOCK();
        vTaskDelay(pdMS_TO_TICKS(140));
        UI_LOCK();
        if (s_idle_visible) idle_sprite(SIGNY_OPEN);
        UI_UNLOCK();
    }
}
#endif

void ui_init(uint8_t orientation) {
    gpio_config_t io = {.mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    io.pin_bit_mask = gpio_bit(BTN) | gpio_bit(BTN_REJ) | gpio_bit(BTN_NXT);
    gpio_config(&io);
    s_ui = xSemaphoreCreateMutex();
    s_disp = disp_init(orientation) == 0;
    s_remote = xQueueCreate(4, sizeof(remote_ev_t));
    lay_begin();
    box_t *b = lay_box(); b->fill = 1;
    L_center(b, P_PURPLE, "POKESIGNER"); L_center(b, P_INK, "iniciando...");
    lay_render();
    if (s_disp) {
        int w = 16 * SPLASH_SC, x = (LCD_W - w) / 2;
#if MASCOT_ANIM
        disp_sprite(SIGNY_OPEN, x, SPLASH_Y, w, w, 0, 0, SPLASH_SC, 0, P_WHITE);
        vTaskDelay(pdMS_TO_TICKS(500));
        disp_sprite(SIGNY_BLINK, x, SPLASH_Y, w, w, 0, 0, SPLASH_SC, 0, P_WHITE);
        vTaskDelay(pdMS_TO_TICKS(150));
        disp_sprite(SIGNY_HAPPY, x, SPLASH_Y, w, w, 0, 0, SPLASH_SC, 0, P_WHITE);
        xTaskCreate(blink_task, "blink", 6144, NULL, 2, NULL);
#else
        disp_sprite(SIGNY_HAPPY, x, SPLASH_Y, w, w, 0, 0, SPLASH_SC, 0, P_WHITE);
#endif
    }
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
    UI_LOCK();
    char b1[24], b2[24];
    lay_begin();
    box_t *b = lay_box(); b->fill = 1;
    L_pair(b, P_PURPLE, "POKESIGNER", P_LAV, TW >= 16 ? "v0.6.2" : "");
    L_blank(b);
    snprintf(b1, sizeof(b1), "ID  %08" PRIX32, s_fp);
    L_text(b, P_INK, b1);
    const char *sfx = strchr(s_name, '-');
    snprintf(b2, sizeof(b2), "BLE %s", sfx ? sfx + 1 : s_name);
    L_text(b, P_INK, b2);
    L_blank(b);
    if (link == UI_SECURE) L_text(b, P_GREEN, T_ARROW " CONECTADO");
    else if (link == UI_CONNECTED) L_text(b, P_PURPLE, T_ARROW " PAREANDO...");
    else L_text(b, P_INK, T_ARROW " AGUARDANDO");
    L_blank(b);
    L_text(b, P_PURPLE, "Envie um PSBT");
    lay_render();
    idle_sprite(SIGNY_OPEN);
    s_idle_visible = 1;
    UI_UNLOCK();
}

void ui_refresh_idle(void) { ui_idle(s_link); }

void ui_passkey(uint32_t code) {
    if (!s_disp) return;
    char c[8];
    snprintf(c, sizeof(c), "%06" PRIu32, code);
    UI_LOCK();
    lay_begin();
    box_t *b = lay_box(); b->fill = 1;
    L_text(b, P_PURPLE, "PAREAMENTO");
    L_text(b, P_INK, "Digite no celular:");
    L_big(b, P_INK, c);
    L_blank(b);
    L_text(b, P_LAV, "Nao pediu? Ignore.");
    lay_render();
    UI_UNLOCK();
}

static int msg_draw(const char *title, const char *l1, const char *l2, uint16_t color);

// Dialog box anchored at the bottom of the screen (classic RPG text box).
void ui_msg(const char *title, const char *l1, const char *l2, uint16_t color) {
    if (s_disp) msg_draw(title, l1, l2, color);
}

// Draws the dialog box; returns its top grid row.
static int msg_draw(const char *title, const char *l1, const char *l2, uint16_t color) {
    int pal = color == C_RED ? P_RED : color == C_GREEN ? P_GREEN : color == C_ORANGE ? P_RED : P_PURPLE;
    UI_LOCK();
    s_idle_visible = 0;
    lay_begin();
    box_t *b = lay_box();
    L_text(b, pal, title);
    if (l1 && l1[0]) L_text(b, P_INK, l1);
    if (l2 && l2[0]) L_text(b, P_INK, l2);
    g_clear();
    int h = box_inner(b) + 2;
    if (h > GH) h = GH;
    draw_box(b, GH - h, GH - 1);
    g_show();
    UI_UNLOCK();
    return GH - h;
}

// Celebration after a successful signature: mascot blinks, jumps and sparkles above a dialog box.
void ui_celebrate(const char *l1, const char *l2) {
    if (!s_disp) return;
    s_modal = 1;   // keep BLE status changes from redrawing the idle screen mid-animation
    int top = msg_draw("PSBT ASSINADO!", l1, l2, C_GREEN);
    // fit mascot + jump + sparkle margin above the dialog box
    int avail = top * 8 * TSC - 4, sc = CELEB_SC, sw, jump, pad;
    for (;;) {
        sw = 16 * sc; jump = 3 * sc; pad = 3 * sc;
        if (sw + jump + pad <= avail || sc == 1) break;
        sc--;
    }
    int bw = sw + 2 * pad, bh = sw + jump + pad, bx = (LCD_W - bw) / 2;
    int by = TOPM + 2 + (avail - bh) / 2;
    if (by < TOPM) by = TOPM;
    static const struct { const char *const *f; int up; int sp; int ms; } SEQ[] = {
        {SIGNY_OPEN, 0, 0, 250}, {SIGNY_BLINK, 0, 0, 120}, {SIGNY_OPEN, 0, 0, 150},
        {SIGNY_HAPPY, 1, 1, 170}, {SIGNY_HAPPY, 0, 2, 170}, {SIGNY_HAPPY, 1, 1, 170},
        {SIGNY_HAPPY, 0, 2, 170}, {SIGNY_HAPPY, 1, 1, 170}, {SIGNY_HAPPY, 0, 2, 300},
        {SIGNY_BLINK, 0, 0, 120}, {SIGNY_HAPPY, 0, 0, 500},
    };
#if MASCOT_ANIM
    for (size_t i = 0; i < sizeof(SEQ) / sizeof(SEQ[0]); i++) {
        UI_LOCK();
        disp_sprite(SEQ[i].f, bx, by, bw, bh, pad, pad + (SEQ[i].up ? 0 : jump), sc, SEQ[i].sp, P_LAV);
        UI_UNLOCK();
        vTaskDelay(pdMS_TO_TICKS(SEQ[i].ms));
    }
#else
    (void)SEQ;
    UI_LOCK();
    disp_sprite(SIGNY_HAPPY, bx, by, bw, bh, pad, pad + jump, sc, 1, P_LAV);   // static, smiling
    UI_UNLOCK();
    vTaskDelay(pdMS_TO_TICKS(2000));
#endif
    s_modal = 0;
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
    int per = GH - 4;              // borders + title + footer
    if (per > 12) per = 12;
    int pages = (n + per - 1) / per;
    for (int pg = 0; pg < pages; pg++) {
        char t[24];
        lay_begin();
        box_t *b = lay_box(); b->fill = 1;
        snprintf(t, sizeof(t), "SEED %d/%d", pg + 1, pages);
        L_pair(b, P_PURPLE, t, P_RED, "ANOTE!");
        for (int i = pg * per; i < n && i < pg * per + per; i++) {
            snprintf(t, sizeof(t), "%2d %s", i + 1, words[i]);
            L_text(b, P_INK, t);
        }
        strcpy(b->foot_r, T_DOWN);
        UI_LOCK();
        lay_render();
        UI_UNLOCK();
        memset(s_boxes, 0, sizeof(s_boxes));
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

static void footer(box_t *b, int idx, int total) {
    snprintf(b->foot_l, sizeof(b->foot_l), "%d/%d", idx + 1, total);
    strcpy(b->foot_r, T_DOWN);
}

// Pages: 0 encounter/summary, 1..K destinations, [change], confirm (with the code).
static void build_page(const bs_psbt_t *p, int idx, int total, const char *code) {
    size_t K = count_dest(p);
    int has_change = p->change_total > 0;
    char t[72];
    lay_begin();
    if (idx == 0) {
        box_t *b = lay_box();
        L_pair(b, P_PURPLE, "PSBT", p->testnet ? P_RED : P_BLUE, p->testnet ? "TESTNET" : "MAINNET");
        snprintf(t, sizeof(t), "ASSINA %u DE %u", (unsigned)p->n_sign, (unsigned)p->n_in);
        L_text(b, P_INK, t);
        if (p->fee_known) snprintf(t, sizeof(t), "TAXA %" PRIu64 " sat", p->fee);
        else snprintf(t, sizeof(t), "TAXA ???");
        L_text(b, fee_high(p) || !p->fee_known ? P_RED : P_INK, t);
        box_t *d = lay_box();
        snprintf(t, sizeof(t), "Um PSBT selvagem apareceu! (%u dest)", (unsigned)K);
        L_text(d, P_INK, t);
        footer(d, idx, total);
    } else if (idx <= (int)K) {
        size_t k = 0, i;
        for (i = 0; i < p->n_out; i++) if (!p->out[i].is_change && ++k == (size_t)idx) break;
        const bs_output_t *o = &p->out[i];
        box_t *b = lay_box();
        snprintf(t, sizeof(t), "PARA %d/%u", idx, (unsigned)K);
        L_text(b, P_PURPLE, t);
        char addr[100];
        if (bs_script_to_addr(o->spk, o->spk_len, p->testnet, addr, sizeof(addr)) == 0) {
            L_addr(b, addr);
        } else if (o->spk_len && o->spk[0] == 0x6a) {
            L_text(b, P_RED, "OP_RETURN");
            snprintf(t, sizeof(t), "%u bytes", (unsigned)o->spk_len);
            L_text(b, P_INK, t);
        } else {
            L_text(b, P_RED, "SCRIPT ESTRANHO!");
        }
        L_blank(b);
        L_text(b, P_PURPLE, "VALOR");
        L_amount(b, P_GREEN, o->amount);
        footer(b, idx, total);
    } else if (has_change && idx == (int)K + 1) {
        box_t *b = lay_box();
        L_text(b, P_PURPLE, "TROCO");
        L_blank(b);
        L_text(b, P_INK, "Volta para voce:");
        L_amount(b, P_GREEN, p->change_total);
        L_text(b, P_LAV, "(verificado)");
        footer(b, idx, total);
    } else {
        box_t *b = lay_box();
        L_text(b, P_PURPLE, "ENVIAR");
        L_amount(b, P_GREEN, p->out_total - p->change_total);
        if (p->fee_known) {
            snprintf(t, sizeof(t), "TAXA %" PRIu64 " sat%s", p->fee, fee_high(p) ? " !" : "");
            L_text(b, fee_high(p) ? P_RED : P_INK, t);
        }
        box_t *c = lay_box(); c->fill = 1;
        L_big(c, P_INK, code);
        L_pair(c, P_GREEN, TW >= 18 ? "OK:BOOT2s/codigo" : "OK:BOOT/cod", P_RED, "X");
    }
    UI_LOCK();
    lay_render();
    UI_UNLOCK();
}

int ui_review(const bs_psbt_t *p, int timeout_ms, int (*abort_cb)(void)) {
    remote_ev_t ev;
    if (s_remote) xQueueReset(s_remote);
    s_reviewing = 1;
    if (!s_disp) {
        // Without a screen there is no trusted channel for a code: only the button approves.
        int result = -1;
        for (;;) {
            int r = wait_input(2000, timeout_ms, abort_cb, &ev);
            if (r == BTN_REMOTE) {
                if (ev.cmd == CMD_CANCEL) { result = 0; break; }
                status("sem tela: aprove segurando o botao BOOT");
                continue;
            }
            result = r == BTN_LONG ? 1 : (r == BTN_SHORT || r == BTN_REJECT) ? 0 : -1;
            break;
        }
        s_reviewing = 0;
        return result;
    }
    s_modal = 1;
    char code[8];
    snprintf(code, sizeof(code), "%06" PRIu32, esp_random() % 1000000);
    int total = 1 + (int)count_dest(p) + (p->change_total > 0) + 1;
    int64_t t0 = esp_timer_get_time();
    int idx = 0, result = -1, wrong = 0;
    char msg[64];
    for (;;) {
        build_page(p, idx, total, code);
        snprintf(msg, sizeof(msg), "pagina %d/%d%s", idx + 1, total,
                 idx == total - 1 ? ": digite o codigo da tela para aprovar" : "");
        status(msg);
        int left = timeout_ms - (int)((esp_timer_get_time() - t0) / 1000);
        if (left <= 0) break;
        int r = wait_input(2000, left, abort_cb, &ev);
        if (r == BTN_TIMEOUT) break;
        if (r == BTN_REJECT) { result = 0; break; }
        if (r == BTN_REMOTE) {
            if (ev.cmd == CMD_CANCEL) { result = 0; break; }
            if (ev.cmd == CMD_NEXT) { if (idx < total - 1) idx++; continue; }
            if (ev.cmd == CMD_PREV) { if (idx > 0) idx--; continue; }
            if (ev.cmd == CMD_CONFIRM) {
                if (idx != total - 1) { status("revise todas as paginas antes de aprovar"); continue; }
                if (strcmp(ev.code, code) == 0) { result = 1; break; }
                if (++wrong >= 3) { status("3 codigos errados: transacao rejeitada"); result = 0; break; }
                snprintf(msg, sizeof(msg), "codigo incorreto (%d de 3)", wrong);
                status(msg);
                continue;
            }
            continue;
        }
        if (idx < total - 1) { idx++; continue; }  // every page must be seen
        result = (r == BTN_LONG) ? 1 : 0;
        break;
    }
    memset(code, 0, sizeof(code));
    s_reviewing = 0;
    if (result == 1) ui_msg("APROVADO!", "Assinando o PSBT...", NULL, C_GREEN);
    else if (result == 0) ui_msg("REJEITADO", "Voce recusou o PSBT.", NULL, C_RED);
    else ui_msg("FUGIU!", "O PSBT fugiu! (tempo esgotado ou desconexao)", NULL, C_ORANGE);
    vTaskDelay(pdMS_TO_TICKS(1200));
    s_modal = 0;
    return result;
}
