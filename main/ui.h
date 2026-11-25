#pragma once
#include <stdint.h>
#include "bs_signer.h"

enum { BTN_TIMEOUT = -1, BTN_SHORT = 0, BTN_LONG = 1, BTN_REMOTE = 2, BTN_REJECT = 3 };

// Remote control from the phone (same codes as the BLE commands 0x04..0x07).
void ui_remote_event(uint8_t cmd, const uint8_t *payload, size_t len);
// Informative messages for the phone during a review (e.g. "codigo incorreto").
void ui_set_status_cb(void (*cb)(const char *msg));

// message colors for ui_msg
#define C_WHITE  0
#define C_GREEN  1
#define C_RED    2
#define C_YELLOW 3
#define C_ORANGE 4

void ui_init(uint8_t orientation);
void ui_rotate(uint8_t orientation);
int  ui_has_display(void);

// Waits for a press. BTN_LONG if held >= hold_ms. abort_cb may be NULL.
int  ui_wait_button(int hold_ms, int timeout_ms, int (*abort_cb)(void));

typedef enum { UI_ADV, UI_CONNECTED, UI_SECURE } ui_link_t;
void ui_set_identity(const char *name, uint32_t fingerprint);
void ui_idle(ui_link_t link);
void ui_refresh_idle(void);     // redraws idle screen unless a modal screen is up
void ui_passkey(uint32_t code);
void ui_msg(const char *title, const char *l1, const char *l2, uint16_t color);
void ui_seed_words(const char *mnemonic);   // pages through the words with BOOT

// Paged review. 1 = approved, 0 = rejected, -1 = timeout/abort.
int  ui_review(const bs_psbt_t *p, int timeout_ms, int (*abort_cb)(void));
