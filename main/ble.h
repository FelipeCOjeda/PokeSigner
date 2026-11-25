#pragma once
#include <stdint.h>
#include <stddef.h>

// Protocol frame: [cmd:1][len:4 LE][payload:len], split across BLE writes /
// notifications of any size.
#define CMD_GET_INFO   0x01   // payload: [network:1 (0=main,1=test)] [account:4 LE] (both optional)
#define CMD_SIGN_PSBT  0x02   // payload: raw PSBT bytes (binary, not base64)
#define CMD_PING       0x03

#define RSP_INFO       0x81   // JSON
#define RSP_SIGNED     0x82   // raw signed PSBT
#define RSP_PONG       0x83   // text
#define RSP_STATUS     0x90   // text, informative (e.g. waiting for button)
#define RSP_ERROR      0xFF   // text

#define PROTO_MAX_PAYLOAD (32 * 1024)

// Called from the BLE stack when a full request arrived. Must return quickly.
typedef void (*ble_request_cb)(uint8_t cmd, const uint8_t *payload, size_t len);

void ble_start(const char *name, ble_request_cb cb);
int  ble_send(uint8_t cmd, const uint8_t *payload, size_t len);
int  ble_connected(void);
void ble_release_rx(void);     // worker finished with the request buffer
void ble_forget_bonds(void);
