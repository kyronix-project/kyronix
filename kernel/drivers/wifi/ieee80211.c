#include "ieee80211.h"
#include "../../arch/x86_64/cpu.h"
#include "../../arch/x86_64/pit.h"
#include "../../lib/log.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../usb/usb.h"

static void put_fc(uint8_t *p, uint16_t fc) {
    p[0] = fc & 0xFF;
    p[1] = (fc >> 8) & 0xFF;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static uint32_t crc32_ieee(const uint8_t *data, int len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320u & -(int) (crc & 1));
    }
    return ~crc;
}

static void mgmt_hdr(uint8_t *out, uint16_t subtype, const uint8_t *da, const uint8_t *sa,
                     const uint8_t *bssid) {
    put_fc(out, IEEE80211_FC_TYPE_MGMT | subtype);
    put16(out + 2, 0);
    memcpy(out + 4, da, 6);
    memcpy(out + 10, sa, 6);
    memcpy(out + 16, bssid, 6);
    put16(out + 22, 0);
}

uint16_t wifi_build_auth(wifi_t *w, uint8_t *out) {
    mgmt_hdr(out, IEEE80211_FC_SUBTYPE_AUTH, w->bssid, w->nd.mac, w->bssid);
    put16(out + 24, 0);
    put16(out + 26, 1);
    put16(out + 28, 0);
    uint32_t fcs = crc32_ieee(out, 30);
    put32(out + 30, fcs);
    return 34;
}

uint16_t wifi_build_assoc_req(wifi_t *w, uint8_t *out) {
    mgmt_hdr(out, IEEE80211_FC_SUBTYPE_ASSOC_REQ, w->bssid, w->nd.mac, w->bssid);
    int pos = 24;
    put16(out + pos, 0x0431);
    pos += 2;
    put16(out + pos, 10);
    pos += 2;
    out[pos++] = 0x00;
    out[pos++] = w->ssid_len;
    memcpy(out + pos, w->ssid, w->ssid_len);
    pos += w->ssid_len;
    out[pos++] = 0x01;
    out[pos++] = 8;
    out[pos++] = 0x82;
    out[pos++] = 0x84;
    out[pos++] = 0x8B;
    out[pos++] = 0x96;
    out[pos++] = 0x0C;
    out[pos++] = 0x12;
    out[pos++] = 0x18;
    out[pos++] = 0x24;
    out[pos++] = 0x03;
    out[pos++] = 0x01;
    out[pos++] = w->channel;
    out[pos++] = 0x30;
    out[pos++] = 22;
    uint8_t rsn[] = {0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC,
                     0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00, 0x00, 0x00};
    memcpy(out + pos, rsn, 22);
    pos += 22;
    uint32_t fcs = crc32_ieee(out, pos);
    put32(out + pos, fcs);
    return (uint16_t) (pos + 4);
}

uint16_t wifi_build_probe_req(wifi_t *w, uint8_t *out) {
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    mgmt_hdr(out, IEEE80211_FC_SUBTYPE_PROBE_REQ, bcast, w->nd.mac, bcast);
    int pos = 24;
    out[pos++] = 0x00;
    out[pos++] = 0;
    out[pos++] = 0x01;
    out[pos++] = 4;
    out[pos++] = 0x82;
    out[pos++] = 0x84;
    out[pos++] = 0x8B;
    out[pos++] = 0x96;
    uint32_t fcs = crc32_ieee(out, pos);
    put32(out + pos, fcs);
    return (uint16_t) (pos + 4);
}

int wifi_send_eapol(wifi_t *w, const uint8_t *eapol, int len) {
    uint8_t frame[512];
    int pos = 0;
    put_fc(frame, IEEE80211_FC_TYPE_DATA | IEEE80211_FC_SUBTYPE_DATA | IEEE80211_FC_TODS);
    put16(frame + 2, 0);
    memcpy(frame + 4, w->bssid, 6);
    memcpy(frame + 10, w->nd.mac, 6);
    memcpy(frame + 16, w->bssid, 6);
    put16(frame + 22, 0);
    pos = 24;
    memcpy(frame + pos, w->bssid, 6);
    pos += 6;
    memcpy(frame + pos, w->nd.mac, 6);
    pos += 6;
    put16(frame + pos, 0x888E);
    pos += 2;
    memcpy(frame + pos, eapol, len);
    pos += len;
    uint32_t fcs = crc32_ieee(frame, pos);
    put32(frame + pos, fcs);
    return w->hw.tx(w->hw.hw_priv, frame, (uint16_t) (pos + 4));
}

static void wifi_parse_scan(wifi_t *w, const uint8_t *frame, uint16_t len, int8_t rssi) {
    if (len < 38) return;
    uint16_t fc = frame[0] | (frame[1] << 8);
    uint16_t subtype = fc & 0xF0;
    if (subtype != IEEE80211_FC_SUBTYPE_BEACON && subtype != IEEE80211_FC_SUBTYPE_PROBE_RESP)
        return;

    const uint8_t *bssid = frame + 16;
    const uint8_t *body = frame + 24;
    uint16_t body_len = (uint16_t) (len - 24);
    if (body_len < 12) return;

    const uint8_t *p = body + 12;
    const uint8_t *end = body + body_len;
    uint8_t ssid[IEEE80211_SSID_MAX] = {0};
    uint8_t ssid_len = 0;
    uint8_t channel = 0;
    bool rsn = false;

    while (p + 2 <= end) {
        uint8_t id = p[0];
        uint8_t elen = p[1];
        if (p + 2 + elen > end) break;
        if (id == 0 && elen <= IEEE80211_SSID_MAX) {
            memcpy(ssid, p + 2, elen);
            ssid_len = elen;
        } else if (id == 3 && elen >= 1) {
            channel = p[2];
        } else if (id == 48) {
            rsn = true;
        }
        p += 2 + elen;
    }

    for (int i = 0; i < w->scan_count; i++) {
        if (memcmp(w->scan[i].bssid, bssid, 6) == 0) return;
    }
    if (w->scan_count >= IEEE80211_MAX_SCAN) return;
    wifi_scan_result_t *r = &w->scan[w->scan_count++];
    memset(r, 0, sizeof(*r));
    memcpy(r->ssid, ssid, ssid_len);
    r->ssid_len = ssid_len;
    memcpy(r->bssid, bssid, 6);
    r->channel = channel;
    r->rssi = rssi;
    r->rsn = rsn;
    log_info("wifi: scan found '%.*s' ch %d rssi %d %s", ssid_len, ssid, channel, rssi,
             rsn ? "WPA2" : "open");
}

static int wifi_data_to_eth(wifi_t *w, const uint8_t *frame, uint16_t len, uint8_t *eth,
                            uint16_t *eth_len) {
    uint16_t fc = frame[0] | (frame[1] << 8);
    bool tods = (fc & IEEE80211_FC_TODS) != 0;
    bool fromds = (fc & IEEE80211_FC_FROMDS) != 0;
    bool qos = (fc & 0xF0) == IEEE80211_FC_SUBTYPE_QOS_DATA;
    bool prot = (fc & IEEE80211_FC_PROTECTED) != 0;

    const uint8_t *a1 = frame + 4;
    const uint8_t *a2 = frame + 10;
    const uint8_t *a3 = frame + 16;
    const uint8_t *payload = frame + 24;
    if (qos) payload += 2;
    if (len < (uint16_t) (payload - frame + 12)) return -1;

    const uint8_t *da = a1;
    const uint8_t *sa = a3;
    if (tods) {
        da = a3;
        sa = a2;
    }
    if (fromds && tods) {
        payload += 6;
        if (len < (uint16_t) (payload - frame + 12)) return -1;
        da = payload - 6;
        sa = payload;
        payload += 0;
    }

    uint16_t plen = (uint16_t) (len - (payload - frame) - 4);
    if (prot) {
        if (!w->wpa.gtk_ok || plen < 16) return -1;
        const uint8_t *ccmp = payload;
        uint64_t pn = 0;
        for (int i = 0; i < 6; i++) pn |= (uint64_t) ccmp[7 - i] << (i * 8);
        uint8_t nonce[13];
        nonce[0] = 0;
        memcpy(nonce + 1, sa, 6);
        for (int i = 0; i < 6; i++) nonce[7 + i] = (uint8_t) (pn >> (i * 8));
        int data_len = plen - 16;
        uint8_t *plain = kmalloc(data_len);
        if (!plain) return -1;
        memcpy(plain, ccmp + 8, data_len);
        if (aes_ccmp_decrypt(w->wpa.gtk, nonce, frame, (int) (payload - frame), plain, data_len,
                             ccmp + 8 + data_len)) {
            kfree(plain);
            return -1;
        }
        payload = plain;
        plen = (uint16_t) data_len;
        memcpy(eth, da, 6);
        memcpy(eth + 6, sa, 6);
        uint16_t ethtype = (payload[6] << 8) | payload[7];
        put16(eth + 12, ethtype);
        memcpy(eth + 14, payload + 8, plen - 8);
        *eth_len = (uint16_t) (14 + plen - 8);
        kfree(plain);
        return 0;
    }

    memcpy(eth, da, 6);
    memcpy(eth + 6, sa, 6);
    uint16_t ethtype = (payload[6] << 8) | payload[7];
    put16(eth + 12, ethtype);
    memcpy(eth + 14, payload + 8, plen - 8);
    *eth_len = (uint16_t) (14 + plen - 8);
    return 0;
}

int wifi_send_data_frame(wifi_t *w, const uint8_t *eth, uint16_t eth_len) {
    if (w->state != WIFI_STATE_CONNECTED || !w->wpa.ptk_ok) return -1;

    uint8_t frame[2048];
    int pos = 0;
    put_fc(frame, IEEE80211_FC_TYPE_DATA | IEEE80211_FC_SUBTYPE_DATA | IEEE80211_FC_TODS |
                      IEEE80211_FC_PROTECTED);
    put16(frame + 2, 0);
    memcpy(frame + 4, w->bssid, 6);
    memcpy(frame + 10, w->nd.mac, 6);
    memcpy(frame + 16, eth, 6);
    put16(frame + 22, 0);
    pos = 24;

    uint8_t hdr_aad_len = (uint8_t) pos;
    uint8_t aad[24];
    memcpy(aad, frame, 24);
    aad[1] &= 0xC7;
    aad[22] &= 0x0F;
    aad[23] = 0;

    uint64_t pn = ++w->pn;
    uint8_t ccmp_hdr[8];
    ccmp_hdr[0] = (uint8_t) (pn & 0xFF);
    ccmp_hdr[1] = (uint8_t) ((pn >> 8) & 0xFF);
    ccmp_hdr[2] = 0;
    ccmp_hdr[3] = 0x20;
    ccmp_hdr[4] = (uint8_t) ((pn >> 16) & 0xFF);
    ccmp_hdr[5] = (uint8_t) ((pn >> 24) & 0xFF);
    ccmp_hdr[6] = (uint8_t) ((pn >> 32) & 0xFF);
    ccmp_hdr[7] = (uint8_t) ((pn >> 40) & 0xFF);

    uint8_t llc[8];
    memcpy(llc, eth + 6, 6);
    put16(llc + 6, (uint16_t) ((eth[12] << 8) | eth[13]));
    int data_len = 8 + eth_len - 14;
    uint8_t *data = kmalloc(data_len);
    if (!data) return -1;
    memcpy(data, llc, 8);
    memcpy(data + 8, eth + 14, eth_len - 14);

    uint8_t nonce[13];
    nonce[0] = 0;
    memcpy(nonce + 1, w->nd.mac, 6);
    for (int i = 0; i < 6; i++) nonce[7 + i] = (uint8_t) (pn >> (i * 8));

    uint8_t mic[8];
    aes_ccmp_encrypt(w->wpa.ptk + WPA_KCK_LEN + WPA_KEK_LEN, nonce, aad, hdr_aad_len, data,
                     data_len, mic);

    memcpy(frame + pos, ccmp_hdr, 8);
    pos += 8;
    memcpy(frame + pos, data, data_len);
    pos += data_len;
    memcpy(frame + pos, mic, 8);
    pos += 8;
    uint32_t fcs = crc32_ieee(frame, pos);
    put32(frame + pos, fcs);
    pos += 4;

    kfree(data);
    return w->hw.tx(w->hw.hw_priv, frame, (uint16_t) pos);
}

void wifi_rx_frame(wifi_t *w, const uint8_t *frame, uint16_t len) {
    if (len < 24) return;
    uint16_t fc = frame[0] | (frame[1] << 8);
    uint16_t type = fc & 0x0C;
    uint16_t subtype = fc & 0xF0;

    if (type == IEEE80211_FC_TYPE_MGMT) {
        if (subtype == IEEE80211_FC_SUBTYPE_BEACON ||
            subtype == IEEE80211_FC_SUBTYPE_PROBE_RESP) {
            wifi_parse_scan(w, frame, len, -60);
        } else if (subtype == IEEE80211_FC_SUBTYPE_AUTH && w->state == WIFI_STATE_AUTHENTICATING) {
            if (len >= 30 && frame[28] == 0 && frame[29] == 0) {
                uint8_t assoc[256];
                uint16_t alen = wifi_build_assoc_req(w, assoc);
                w->hw.tx(w->hw.hw_priv, assoc, alen);
                w->state = WIFI_STATE_ASSOCIATING;
                log_info("wifi: auth ok, associating");
            }
        } else if (subtype == IEEE80211_FC_SUBTYPE_ASSOC_RESP &&
                   w->state == WIFI_STATE_ASSOCIATING) {
            if (len >= 30 && frame[26] == 0 && frame[27] == 0) {
                w->aid = frame[28] | ((frame[29] & 0x3F) << 8);
                w->state = WIFI_STATE_EAPOL_1;
                log_info("wifi: associated, AID=%d, waiting EAPOL", w->aid);
            }
        }
        return;
    }

    if (type != IEEE80211_FC_TYPE_DATA) return;

    if (w->state >= WIFI_STATE_EAPOL_1 && w->state < WIFI_STATE_CONNECTED) {
        const uint8_t *payload = frame + 24;
        if (len < 24 + 8 + 5) return;
        uint16_t ethtype = (payload[6] << 8) | payload[7];
        if (ethtype != 0x888E) return;
        const uint8_t *eapol = payload + 8;
        int eapol_len = len - 24 - 8 - 4;
        if (eapol_len < 4) return;

        if (w->state == WIFI_STATE_EAPOL_1) {
            uint8_t m2[256];
            int m2_len;
            memcpy(w->wpa.ap_mac, frame + 10, 6);
            memcpy(w->wpa.sta_mac, w->nd.mac, 6);
            memcpy(w->wpa.bssid, w->bssid, 6);
            if (wpa_build_eapol_m2(&w->wpa, eapol, eapol_len, m2, &m2_len) == 0) {
                wifi_send_eapol(w, m2, m2_len);
                w->state = WIFI_STATE_EAPOL_3;
                log_info("wifi: EAPOL M2 sent");
            }
        } else if (w->state == WIFI_STATE_EAPOL_3) {
            uint8_t m4[128];
            int m4_len;
            if (wpa_process_eapol_m3(&w->wpa, eapol, eapol_len, m4, &m4_len) == 0) {
                wifi_send_eapol(w, m4, m4_len);
                w->state = WIFI_STATE_CONNECTED;
                w->pn = 0;
                log_info("wifi: WPA2 handshake complete, connected!");
            }
        }
        return;
    }

    if (w->state == WIFI_STATE_CONNECTED) {
        uint8_t eth[2048];
        uint16_t eth_len;
        if (wifi_data_to_eth(w, frame, len, eth, &eth_len) == 0)
            netdev_receive(&w->nd, eth, eth_len);
    }
}

int wifi_connect(wifi_t *w, const char *ssid, const char *passphrase) {
    int slen = 0;
    while (ssid[slen] && slen < IEEE80211_SSID_MAX) slen++;
    memcpy(w->ssid, ssid, slen);
    w->ssid[slen] = 0;
    w->ssid_len = (uint8_t) slen;
    int plen = 0;
    while (passphrase[plen] && plen < 63) plen++;
    memcpy(w->passphrase, passphrase, plen);
    w->passphrase[plen] = 0;

    wpa_derive_psk(&w->wpa, passphrase, (const uint8_t *) ssid, slen);

    w->scan_count = 0;
    w->state = WIFI_STATE_SCANNING;
    for (int ch = 1; ch <= 13; ch++) {
        w->hw.set_channel(w->hw.hw_priv, ch);
        uint8_t probe[128];
        uint16_t plen2 = wifi_build_probe_req(w, probe);
        w->hw.tx(w->hw.hw_priv, probe, plen2);
        usb_msleep(40);
    }

    for (int i = 0; i < w->scan_count; i++) {
        if (w->scan[i].ssid_len == slen && memcmp(w->scan[i].ssid, ssid, slen) == 0) {
            memcpy(w->bssid, w->scan[i].bssid, 6);
            w->channel = w->scan[i].channel;
            w->hw.set_channel(w->hw.hw_priv, w->channel);
            uint8_t auth[64];
            uint16_t alen = wifi_build_auth(w, auth);
            w->hw.tx(w->hw.hw_priv, auth, alen);
            w->state = WIFI_STATE_AUTHENTICATING;
            log_info("wifi: authenticating to '%s' on ch %d", ssid, w->channel);
            return 0;
        }
    }
    log_warn("wifi: SSID '%s' not found in %d scan results", ssid, w->scan_count);
    w->state = WIFI_STATE_IDLE;
    return -1;
}

int wifi_scan_count(wifi_t *w) { return w->scan_count; }
const wifi_scan_result_t *wifi_scan_get(wifi_t *w, int idx) {
    if (idx < 0 || idx >= w->scan_count) return NULL;
    return &w->scan[idx];
}
bool wifi_connected(wifi_t *w) { return w->state == WIFI_STATE_CONNECTED; }

int wifi_nd_send(netdev_t *nd, const uint8_t *frame, uint16_t len) {
    wifi_t *w = (wifi_t *) nd->priv;
    return wifi_send_data_frame(w, frame, len);
}

void wifi_poll(netdev_t *nd) { (void) nd; }

void wifi_init_dev(wifi_t *w, const char *name, const uint8_t *mac, wifi_hw_ops_t *hw) {
    memset(w, 0, sizeof(*w));
    int i = 0;
    while (name[i] && i < NETDEV_NAME_MAX - 1) {
        w->nd.name[i] = name[i];
        i++;
    }
    w->nd.name[i] = 0;
    memcpy(w->nd.mac, mac, 6);
    w->nd.send = wifi_nd_send;
    w->nd.poll = wifi_poll;
    w->nd.priv = w;
    w->hw = *hw;
    w->state = WIFI_STATE_IDLE;
}
