// PokeSigner — BLE PSBT signer for ESP32-C6 / ESP32-C3 boards with a screen.
// Seed setup and confirmations happen on the device (USB serial + BOOT button);
// the phone only sends PSBTs and receives signatures over an authenticated
// BLE link.
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "bootloader_random.h"
#include "soc/usb_serial_jtag_struct.h"

#include "bs_signer.h"
#include "bs_bip39.h"
#include "bs_crypto.h"
#include "ble.h"
#include "ui.h"
#include "display.h"
#include "board.h"

#define FW_VERSION      "0.5.0"
#define APPROVE_HOLD_MS 2000
#define CONFIRM_TIMEOUT_MS 120000
#define NVS_NS          "pokesigner"

void bs_platform_random(uint8_t *buf, size_t len) { esp_fill_random(buf, len); }

/* ------------------------------------------------------------ serial I/O */

static void console_init(void) {
    // The host must not be able to reset (or reboot into download mode) a
    // running signer by toggling DTR/RTS. Firmware updates use BOOT+RESET.
#if CONFIG_IDF_TARGET_ESP32C6
    USB_SERIAL_JTAG.chip_rst.usb_uart_chip_rst_dis = 1;   // (the C3 has no such switch)
#endif
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 1024;
    cfg.tx_buffer_size = 1024;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();   // stdout goes through the driver
    setvbuf(stdout, NULL, _IONBF, 0);
}

// Reads one byte straight from the driver. Returns -1 on timeout.
static int con_getc(int timeout_ms) {
    uint8_t c;
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return usb_serial_jtag_read_bytes(&c, 1, t) == 1 ? c : -1;
}

#define RL_MASK  1   // echo '*'
#define RL_EMPTY 2   // accept an empty line
// Reads a line. timeout_ms applies only while nothing was typed yet
// (returns -1 then); -1 = wait forever.
static int read_line_t(char *buf, size_t cap, int flags, int timeout_ms) {
    int masked = flags & RL_MASK;
    size_t n = 0;
    for (;;) {
        int c = con_getc(n == 0 ? timeout_ms : -1);
        if (c < 0) { buf[0] = 0; return -1; }
        if (c == '\r' || c == '\n') {
            if (n == 0 && !(flags & RL_EMPTY)) continue;
            break;
        }
        if ((c == 8 || c == 127) && n) { n--; printf("\b \b"); continue; }
        if (c < 32 || c > 126 || n + 1 >= cap) continue;
        buf[n++] = (char)c;
        putchar(masked ? '*' : c);
    }
    buf[n] = 0;
    putchar('\n');
    return (int)n;
}

static size_t read_line(char *buf, size_t cap, int flags) {
    int n = read_line_t(buf, cap, flags, -1);
    return n < 0 ? 0 : (size_t)n;
}

/* ------------------------------------------------------------ storage */

typedef struct { uint8_t ent[32]; size_t ent_len; char pass[257]; } seed_store_t;

static int seed_load(seed_store_t *s) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t l = sizeof(s->ent), pl = sizeof(s->pass);
    esp_err_t e1 = nvs_get_blob(h, "ent", s->ent, &l);
    esp_err_t e2 = nvs_get_str(h, "pass", s->pass, &pl);
    nvs_close(h);
    if (e1 != ESP_OK || (l != 16 && l != 20 && l != 24 && l != 28 && l != 32)) return -1;
    if (e2 != ESP_OK) s->pass[0] = 0;
    s->ent_len = l;
    return 0;
}

static int seed_save(const seed_store_t *s) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = nvs_set_blob(h, "ent", s->ent, s->ent_len);
    if (e == ESP_OK) e = nvs_set_str(h, "pass", s->pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK ? 0 : -1;
}

static uint8_t orient_load(void) {
    nvs_handle_t h; uint8_t o = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "orient", &o); nvs_close(h); }
    return o & 3;
}

static void orient_save(uint8_t o) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "orient", o); nvs_commit(h); nvs_close(h); }
}

static void seed_erase(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
}

/* ------------------------------------------------------------ seed setup */

static void gen_entropy(uint8_t out[32]) {
    // True RNG needs an entropy source: enable the SAR ADC noise source while
    // the radio is still off, then whiten with SHA-256 over plenty of samples.
    uint8_t pool[256];
    bootloader_random_enable();
    esp_fill_random(pool, sizeof(pool));
    bootloader_random_disable();
    printf("Digite caracteres aleatorios para misturar entropia extra (ou ENTER para pular):\n> ");
    char extra[128] = {0};
    int c; size_t n = 0;
    while ((c = con_getc(-1)) != '\r' && c != '\n') {
        if (n < sizeof(extra) - 1) { extra[n++] = (char)c; putchar('*'); }
    }
    putchar('\n');
    int64_t t = esp_timer_get_time();
    bs_sha256_ctx ctx;
    bs_sha256_init(&ctx);
    bs_sha256_update(&ctx, pool, sizeof(pool));
    bs_sha256_update(&ctx, extra, n);
    bs_sha256_update(&ctx, &t, sizeof(t));
    bs_sha256_final(&ctx, out);
    bs_wipe(pool, sizeof(pool)); bs_wipe(extra, sizeof(extra));
}

static void setup_seed(seed_store_t *s) {
    char line[400], mn[400];
    for (;;) {
        printf("\n===== PokeSigner: configuracao inicial =====\n"
               "[g] gerar nova seed (24 palavras)\n"
               "[i] importar seed existente\n> ");
        if (read_line_t(line, sizeof(line), RL_EMPTY, 5000) < 0) continue;  // reprint menu every 5 s
        if (line[0] == 'g') {
            gen_entropy(s->ent); s->ent_len = 32;
            bs_bip39_from_entropy(s->ent, 32, mn, sizeof(mn));
            char tmp[400];
            int on_serial = !ui_has_display();
#if CONFIG_PS_SEED_SERIAL_ECHO
            on_serial = 1;
#endif
            if (on_serial) {
                printf("\nANOTE EM PAPEL, NA ORDEM. Nao fotografe.\n\n");
                strcpy(tmp, mn);
                int i = 1;
                for (char *w = strtok(tmp, " "); w; w = strtok(NULL, " "), i++)
                    printf("%2d. %-10s%s", i, w, i % 4 == 0 ? "\n" : "  ");
                bs_wipe(tmp, sizeof(tmp));
            }
            if (ui_has_display()) {
                printf("\nAs palavras estao NA TELA do dispositivo. Anote e avance com o botao BOOT.\n");
                ui_seed_words(mn);
            } else {
                printf("\nDepois de anotar, pressione ENTER.");
                read_line(line, sizeof(line), RL_EMPTY);
            }
            printf("\033[2J\033[H");  // clear terminal
            // verify two random words
            int ok = 1;
            for (int k = 0; k < 2 && ok; k++) {
                int idx = esp_random() % 24;
                strcpy(tmp, mn);
                char *w = strtok(tmp, " ");
                for (int j = 0; j < idx; j++) w = strtok(NULL, " ");
                printf("Confirme a palavra #%d: ", idx + 1);
                read_line(line, sizeof(line), 0);
                ok = strcmp(line, w) == 0;
                bs_wipe(tmp, sizeof(tmp));
            }
            if (!ok) { printf("Palavra errada. Recomecando.\n"); bs_wipe(mn, sizeof(mn)); continue; }
        } else if (line[0] == 'i') {
            printf("Digite as palavras separadas por espaco:\n> ");
            read_line(line, sizeof(line), 0);
            int r = bs_bip39_to_entropy(line, s->ent, &s->ent_len);
            bs_wipe(line, sizeof(line));
            printf("\033[2J\033[H");
            if (r) { printf("Seed invalida (%s).\n", r == -2 ? "palavra desconhecida" : r == -3 ? "checksum" : "formato"); continue; }
        } else {
            continue;
        }
        printf("Passphrase BIP39 (ENTER para nenhuma). Ex.: pokemon:74:Geodude\n> ");
        read_line(s->pass, sizeof(s->pass), RL_MASK | RL_EMPTY);
        if (s->pass[0]) {
            printf("Repita a passphrase:\n> ");
            read_line(line, sizeof(line), RL_MASK | RL_EMPTY);
            if (strcmp(line, s->pass)) { printf("Nao confere. Recomecando.\n"); bs_wipe(s, sizeof(*s)); continue; }
            bs_wipe(line, sizeof(line));
        }
        if (seed_save(s)) { printf("ERRO ao gravar na flash.\n"); continue; }
        bs_wipe(mn, sizeof(mn));
        printf("Seed gravada.\n");
        return;
    }
}

/* ------------------------------------------------------------ worker */

typedef struct { uint8_t cmd; const uint8_t *payload; size_t len; } job_t;
static QueueHandle_t s_jobs;
static uint8_t s_out[PROTO_MAX_PAYLOAD + 4096];
static char s_text[4096];
static bs_psbt_t s_psbt;

static void on_request(uint8_t cmd, const uint8_t *payload, size_t len) {
    job_t j = {cmd, payload, len};
    if (xQueueSend(s_jobs, &j, 0) != pdTRUE) ble_release_rx();
}

static int disconnected(void) { return !ble_connected(); }

static void on_control(uint8_t cmd, const uint8_t *payload, size_t len) { ui_remote_event(cmd, payload, len); }

static void phone_status(const char *m) { ble_send(RSP_STATUS, (const uint8_t *)m, strlen(m)); }

static void reply_err(const char *msg) { ble_send(RSP_ERROR, (const uint8_t *)msg, strlen(msg)); }

static void handle_sign(const uint8_t *psbt, size_t len) {
    printf("[PSBT] recebido %u bytes, analisando...\n", (unsigned)len);
    if (len >= 5 && memcmp(psbt, "cHNi", 4) == 0) {
        printf("[PSBT] veio em base64: o app deve mandar os bytes binarios\n");
        reply_err("PSBT veio em base64; envie os bytes binarios (decode antes)");
        return;
    }
    int r = bs_psbt_analyze(psbt, len, &s_psbt);
    if (r) {
        char why[200];
        const char *d = bs_psbt_detail();
        snprintf(why, sizeof(why), "%s%s%s", bs_strerror(r), d[0] ? ": " : "", d);
        printf("[PSBT] recusado: %s\n", why);
        reply_err(why);
        uint32_t ffp;
        char l1[24], l2[24];
        if (r == BS_ERR_NOTHING_TO_SIGN && bs_psbt_foreign_fp(&ffp)) {
            snprintf(l1, sizeof(l1), "PSBT %08" PRIX32, ffp);
            snprintf(l2, sizeof(l2), "SIGN %08" PRIX32, bs_signer_fingerprint());
            ui_msg("OUTRA CARTEIRA!", l1, l2, C_RED);
        } else if (r == BS_ERR_NOTHING_TO_SIGN && bs_psbt_zero_fp()) {
            ui_msg("SEM FINGERPRINT", "Importe a carteira", "pelo PokeSigner.", C_RED);
        } else {
            ui_msg("PSBT RECUSADO", bs_strerror(r), NULL, C_RED);
        }
        vTaskDelay(pdMS_TO_TICKS(2500));
        ui_refresh_idle();
        return;
    }
    bs_psbt_summary(&s_psbt, s_text, sizeof(s_text));
    printf("\n%s\n", s_text);
    if (ui_has_display())
        printf("Revise na tela. BOOT (ou o celular) avanca as paginas; na ultima: segure BOOT 2s ou digite no celular o codigo da tela = APROVAR.\n");
    else
        printf("SEGURE o botao BOOT por 2s para APROVAR. Toque rapido = REJEITAR. (expira em 2 min)\n");
    const char *st = "aguardando confirmacao no dispositivo";
    ble_send(RSP_STATUS, (const uint8_t *)st, strlen(st));
    int ok = ui_review(&s_psbt, CONFIRM_TIMEOUT_MS, disconnected);
    if (ok != 1) {
        printf(ok == 0 ? "[PSBT] REJEITADO\n" : "[PSBT] cancelado (timeout ou desconexao)\n");
        reply_err(ok == 0 ? "rejeitado no dispositivo" : "tempo esgotado");
        ui_refresh_idle();
        return;
    }
    size_t olen;
    r = bs_psbt_sign(&s_psbt, s_out, sizeof(s_out), &olen);
    if (r) { printf("[PSBT] erro ao assinar: %s\n", bs_strerror(r)); reply_err(bs_strerror(r)); ui_refresh_idle(); return; }
    printf("[PSBT] assinado (%u entradas), enviando %u bytes\n", (unsigned)s_psbt.n_sign, (unsigned)olen);
    int sent = ble_send(RSP_SIGNED, s_out, olen);
    ui_msg(sent == 0 ? "ENVIADO" : "FALHA BLE", sent == 0 ? "PSBT assinado" : "reconecte e tente", sent == 0 ? "devolvido ao celular" : "novamente", sent == 0 ? C_GREEN : C_RED);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ui_refresh_idle();
}

static void worker_task(void *arg) {
    job_t j;
    for (;;) {
        if (xQueueReceive(s_jobs, &j, portMAX_DELAY) != pdTRUE) continue;
        switch (j.cmd) {
        case CMD_PING: {
            int n = snprintf(s_text, sizeof(s_text), "PokeSigner %s fp=%08" PRIx32, FW_VERSION, bs_signer_fingerprint());
            ble_send(RSP_PONG, (uint8_t *)s_text, n);
            break;
        }
        case CMD_GET_INFO: {
            int testnet = j.len >= 1 ? j.payload[0] != 0 : 0;
            uint32_t acct = j.len >= 5 ? (j.payload[1] | j.payload[2] << 8 | j.payload[3] << 16 | (uint32_t)j.payload[4] << 24) : 0;
            if (acct >= 0x80000000u) { reply_err("conta invalida"); break; }
            if (bs_signer_get_info(testnet, acct, s_text, sizeof(s_text))) { reply_err("erro"); break; }
            printf("[BLE] xpubs enviados (%s, conta %" PRIu32 ")\n", testnet ? "testnet" : "mainnet", acct);
            ble_send(RSP_INFO, (uint8_t *)s_text, strlen(s_text));
            break;
        }
        case CMD_SIGN_PSBT:
            handle_sign(j.payload, j.len);
            break;
        case CMD_CONFIRM: case CMD_CANCEL: case CMD_NEXT: case CMD_PREV:
            reply_err("nada para confirmar agora");
            break;
        default:
            reply_err("comando desconhecido");
        }
        ble_release_rx();
    }
}

/* ------------------------------------------------------------ serial commands */

static void console_task(void *arg) {
    char line[64];
    for (;;) {
        printf("\nComandos: info | girar | unpair | wipe\n> ");
        read_line(line, sizeof(line), RL_EMPTY);
        if (!strcmp(line, "info")) {
            if (bs_signer_get_info(0, 0, s_text, sizeof(s_text)) == 0) printf("%s\n", s_text);
        } else if (!strcmp(line, "girar")) {
            static uint8_t o = 0xff;
            if (o == 0xff) o = orient_load();
            o = (o + 1) & 3;
            orient_save(o);
            ui_rotate(o);
            printf("Orientacao %u salva. Repita 'girar' ate o texto ficar certo.\n", o);
        } else if (!strcmp(line, "unpair")) {
            ble_forget_bonds();
            printf("Pareamentos BLE apagados.\n");
        } else if (!strcmp(line, "wipe")) {
            printf("APAGAR SEED E PAREAMENTOS? Segure BOOT por 5s para confirmar.\n");
            ui_msg("APAGAR TUDO?", "segure BOOT 5s", "para confirmar", C_RED);
            if (ui_wait_button(5000, 30000, NULL) == BTN_LONG) {
                seed_erase(); ble_forget_bonds(); bs_signer_wipe();
                printf("Apagado. Reiniciando...\n");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            printf("Cancelado.\n");
            ui_refresh_idle();
        }
    }
}

/* ------------------------------------------------------------ main */

void app_main(void) {
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    console_init();
    uint8_t orient = orient_load();
    ui_init(orient);
    vTaskDelay(pdMS_TO_TICKS(1500));  // time for the serial monitor to attach
    printf("\nPokeSigner %s (%s)\n", FW_VERSION, BOARD_NAME);

    seed_store_t st;
    memset(&st, 0, sizeof(st));
    if (seed_load(&st)) {
        ui_msg("CONFIGURAR", "Abra o instalador no", "computador e clique em Conectar.", C_YELLOW);
        setup_seed(&st);
    }
    ui_msg("CARREGANDO", "Derivando as chaves...", NULL, C_WHITE);

    char mn[400];
    uint8_t seed[64];
    printf("Derivando chaves (PBKDF2)...\n");
    bs_bip39_from_entropy(st.ent, st.ent_len, mn, sizeof(mn));
    bs_bip39_seed(mn, st.pass, seed);
    bs_wipe(mn, sizeof(mn)); bs_wipe(&st, sizeof(st));
    if (bs_signer_init(seed) != BS_OK) { printf("Falha ao iniciar o signer\n"); abort(); }
    bs_wipe(seed, sizeof(seed));
    printf("Fingerprint: %08" PRIx32 "\n", bs_signer_fingerprint());

    s_jobs = xQueueCreate(1, sizeof(job_t));
    xTaskCreate(worker_task, "signer", 12288, NULL, 5, NULL);

    uint8_t mac[6]; char name[24];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(name, sizeof(name), "PokeSigner-%02X%02X", mac[4], mac[5]);
    ui_set_identity(name, bs_signer_fingerprint());
    ui_idle(UI_ADV);
    ui_set_status_cb(phone_status);
    ble_start(name, on_request, on_control);

    xTaskCreate(console_task, "console", 6144, NULL, 3, NULL);
}
