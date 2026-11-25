// BLE transport: NimBLE peripheral, one GATT service with RX (write) and TX
// (notify) characteristics. LE Secure Connections + MITM (passkey shown on the
// USB serial console) are required before any write is accepted.
#include "ble.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);

// 6e400001-b5a3-f393-e0a9-50ca5e1d0001 style custom UUIDs (little-endian bytes)
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x01, 0x00, 0x1d, 0x5e, 0xca, 0x50, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x53, 0x50);
static const ble_uuid128_t RX_UUID = BLE_UUID128_INIT(
    0x01, 0x00, 0x1d, 0x5e, 0xca, 0x50, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x53, 0x50);
static const ble_uuid128_t TX_UUID = BLE_UUID128_INIT(
    0x01, 0x00, 0x1d, 0x5e, 0xca, 0x50, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x53, 0x50);

static ble_request_cb s_cb;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static uint16_t s_mtu = 23;
static uint8_t s_own_addr_type;
static char s_name[32];

static uint8_t s_rx[PROTO_MAX_PAYLOAD];
static uint8_t s_hdr[5];
static size_t s_hdr_len, s_rx_len, s_rx_expect;
static volatile int s_busy;

static void advertise(void);

static void rx_reset(void) { s_hdr_len = 0; s_rx_len = 0; s_rx_expect = 0; }

void ble_release_rx(void) { rx_reset(); s_busy = 0; }

int ble_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

static void send_error_now(const char *msg) { ble_send(RSP_ERROR, (const uint8_t *)msg, strlen(msg)); }

static void rx_feed(const uint8_t *d, size_t n) {
    while (n) {
        if (s_busy) { send_error_now("ocupado"); return; }
        if (s_hdr_len < 5) {
            size_t k = 5 - s_hdr_len < n ? 5 - s_hdr_len : n;
            memcpy(s_hdr + s_hdr_len, d, k); s_hdr_len += k; d += k; n -= k;
            if (s_hdr_len < 5) return;
            s_rx_expect = s_hdr[1] | s_hdr[2] << 8 | s_hdr[3] << 16 | (uint32_t)s_hdr[4] << 24;
            if (s_rx_expect > PROTO_MAX_PAYLOAD) { rx_reset(); send_error_now("payload grande demais"); return; }
            s_rx_len = 0;
        }
        size_t k = s_rx_expect - s_rx_len < n ? s_rx_expect - s_rx_len : n;
        memcpy(s_rx + s_rx_len, d, k); s_rx_len += k; d += k; n -= k;
        if (s_rx_len == s_rx_expect) {
            s_busy = 1;
            s_cb(s_hdr[0], s_rx, s_rx_len);
            return;  // extra bytes after a complete frame are dropped
        }
    }
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t tmp[520];
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > sizeof(tmp)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &len)) return BLE_ATT_ERR_UNLIKELY;
        rx_feed(tmp, len);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return 0;  // TX is notify-only
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &RX_UUID.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP |
                         BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,
            },
            {
                .uuid = &TX_UUID.u,
                .access_cb = chr_access,
                .val_handle = &s_tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ |
                         BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN,
            },
            {0}},
    },
    {0}};

int ble_send(uint8_t cmd, const uint8_t *payload, size_t len) {
    uint8_t hdr[5] = {cmd, (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
    size_t total = 5 + len, off = 0;
    while (off < total) {
        if (!ble_connected()) return -1;
        size_t chunk = s_mtu > 3 ? s_mtu - 3 : 20;
        if (chunk > 512) chunk = 512;
        if (chunk > total - off) chunk = total - off;
        uint8_t buf[512];
        for (size_t i = 0; i < chunk; i++) {
            size_t pos = off + i;
            buf[i] = pos < 5 ? hdr[pos] : payload[pos - 5];
        }
        int tries = 0, rc;
        for (;;) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, chunk);
            rc = om ? ble_gatts_notify_custom(s_conn, s_tx_handle, om) : BLE_HS_ENOMEM;
            if (rc == 0) break;
            if ((rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) || ++tries > 200) return -1;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        off += chunk;
    }
    return 0;
}

void ble_forget_bonds(void) {
    ble_store_clear();
    if (ble_connected()) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
}

static int gap_event(struct ble_gap_event *ev, void *arg) {
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle; s_mtu = 23; rx_reset();
            printf("[BLE] conectado\n");
            ui_idle(UI_CONNECTED);
            // Ask the phone to start pairing/encryption right away.
            ble_gap_security_initiate(s_conn);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        printf("[BLE] desconectado\n");
        s_conn = BLE_HS_CONN_HANDLE_NONE; rx_reset();
        ui_idle(UI_ADV);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = ev->mtu.value;
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc d;
        if (ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0) {
            printf("[BLE] canal cifrado (autenticado=%d, pareado=%d)\n",
                   d.sec_state.authenticated, d.sec_state.bonded);
            ui_idle(d.sec_state.authenticated ? UI_SECURE : UI_CONNECTED);
        } else {
            printf("[BLE] falha na criptografia/pareamento (%d)\n", ev->enc_change.status);
            ui_idle(UI_CONNECTED);
        }
        break;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (ev->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000};
            printf("\n*** PAREAMENTO BLE: digite no celular o codigo %06" PRIu32 " ***\n\n", io.passkey);
            ui_passkey(io.passkey);
            ble_sm_inject_io(ev->passkey.conn_handle, &io);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_hs_adv_fields f = {0}, rsp = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&SVC_UUID; f.num_uuids128 = 1; f.uuids128_is_complete = 1;
    rsp.name = (uint8_t *)s_name; rsp.name_len = strlen(s_name); rsp.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) printf("[BLE] erro ao anunciar: %d\n", rc);
}

static void on_sync(void) {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
    printf("[BLE] anunciando como \"%s\"\n", s_name);
}

static void on_reset(int reason) { printf("[BLE] reset do host: %d\n", reason); }

static void host_task(void *arg) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_start(const char *name, ble_request_cb cb) {
    s_cb = cb;
    strncpy(s_name, name, sizeof(s_name) - 1);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_svcs));
    ble_svc_gap_device_name_set(s_name);
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}
