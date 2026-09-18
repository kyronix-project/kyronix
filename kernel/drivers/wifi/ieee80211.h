#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "../netdev.h"
#include "wpa.h"

#define IEEE80211_MAX_SCAN 24
#define IEEE80211_SSID_MAX 32

#define IEEE80211_FC_TYPE_MGMT 0x00
#define IEEE80211_FC_TYPE_DATA 0x02
#define IEEE80211_FC_SUBTYPE_ASSOC_REQ 0x00
#define IEEE80211_FC_SUBTYPE_ASSOC_RESP 0x10
#define IEEE80211_FC_SUBTYPE_PROBE_REQ 0x40
#define IEEE80211_FC_SUBTYPE_PROBE_RESP 0x50
#define IEEE80211_FC_SUBTYPE_BEACON 0x80
#define IEEE80211_FC_SUBTYPE_AUTH 0xB0
#define IEEE80211_FC_SUBTYPE_DEAUTH 0xC0
#define IEEE80211_FC_SUBTYPE_DATA 0x00
#define IEEE80211_FC_SUBTYPE_QOS_DATA 0x80

#define IEEE80211_FC_TODS 0x0100
#define IEEE80211_FC_FROMDS 0x0200
#define IEEE80211_FC_PROTECTED 0x4000

typedef struct {
    uint8_t ssid[IEEE80211_SSID_MAX];
    uint8_t ssid_len;
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    bool rsn;
} wifi_scan_result_t;

typedef struct wifi_hw_ops {
    int (*tx)(void *hw_priv, const uint8_t *frame, uint16_t len);
    int (*set_channel)(void *hw_priv, int channel);
    void *hw_priv;
} wifi_hw_ops_t;

typedef enum {
    WIFI_STATE_IDLE = 0,
    WIFI_STATE_SCANNING,
    WIFI_STATE_AUTHENTICATING,
    WIFI_STATE_ASSOCIATING,
    WIFI_STATE_EAPOL_1,
    WIFI_STATE_EAPOL_3,
    WIFI_STATE_CONNECTED,
} wifi_state_t;

typedef struct {
    netdev_t nd;
    wifi_hw_ops_t hw;
    wifi_state_t state;
    char ssid[IEEE80211_SSID_MAX + 1];
    uint8_t ssid_len;
    char passphrase[64];
    uint8_t bssid[6];
    uint8_t channel;
    wpa_ctx_t wpa;
    wifi_scan_result_t scan[IEEE80211_MAX_SCAN];
    int scan_count;
    uint16_t aid;
    uint64_t pn;
} wifi_t;

void wifi_rx_frame(wifi_t *w, const uint8_t *frame, uint16_t len);
int wifi_connect(wifi_t *w, const char *ssid, const char *passphrase);
int wifi_scan_count(wifi_t *w);
const wifi_scan_result_t *wifi_scan_get(wifi_t *w, int idx);
bool wifi_connected(wifi_t *w);
int wifi_nd_send(netdev_t *nd, const uint8_t *frame, uint16_t len);
void wifi_poll(netdev_t *nd);
void wifi_init_dev(wifi_t *w, const char *name, const uint8_t *mac, wifi_hw_ops_t *hw);

uint16_t wifi_build_auth(wifi_t *w, uint8_t *out);
uint16_t wifi_build_assoc_req(wifi_t *w, uint8_t *out);
uint16_t wifi_build_probe_req(wifi_t *w, uint8_t *out);
int wifi_send_eapol(wifi_t *w, const uint8_t *eapol, int len);
int wifi_send_data_frame(wifi_t *w, const uint8_t *eth, uint16_t eth_len);
