#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WPA_PSK_LEN 32
#define WPA_NONCE_LEN 32
#define WPA_PTK_LEN 64
#define WPA_KCK_LEN 16
#define WPA_KEK_LEN 16
#define WPA_TK_LEN 16
#define WPA_GTK_LEN 16
#define WPA_MIC_LEN 16

void sha1(const uint8_t *data, uint32_t len, uint8_t *digest);
void hmac_sha1(const uint8_t *key, uint32_t key_len, const uint8_t *data, uint32_t len,
               uint8_t *digest);
void pbkdf2_sha1(const char *password, const uint8_t *ssid, uint32_t ssid_len, int iterations,
                 uint8_t *out, uint32_t out_len);

void aes128_encrypt_block(const uint8_t *key, const uint8_t *in, uint8_t *out);
void aes128_decrypt_block(const uint8_t *key, const uint8_t *in, uint8_t *out);
void aes_key_wrap_unwrap(const uint8_t *kek, const uint8_t *wrapped, int wrapped_len,
                         uint8_t *out, int *out_len);
void aes_ccmp_encrypt(const uint8_t *tk, const uint8_t *nonce, const uint8_t *aad, int aad_len,
                      uint8_t *data, int data_len, uint8_t *mic);
int aes_ccmp_decrypt(const uint8_t *tk, const uint8_t *nonce, const uint8_t *aad, int aad_len,
                     uint8_t *data, int data_len, const uint8_t *mic);

typedef struct {
    uint8_t psk[WPA_PSK_LEN];
    uint8_t ptk[WPA_PTK_LEN];
    uint8_t gtk[WPA_GTK_LEN];
    uint8_t anonce[WPA_NONCE_LEN];
    uint8_t snonce[WPA_NONCE_LEN];
    uint8_t ap_mac[6];
    uint8_t sta_mac[6];
    uint8_t bssid[6];
    uint64_t replay_ctr;
    bool ptk_ok;
    bool gtk_ok;
} wpa_ctx_t;

void wpa_derive_psk(wpa_ctx_t *w, const char *passphrase, const uint8_t *ssid, uint32_t ssid_len);
void wpa_gen_snonce(wpa_ctx_t *w);
void wpa_derive_ptk(wpa_ctx_t *w);
int wpa_build_eapol_m2(wpa_ctx_t *w, const uint8_t *m1, int m1_len, uint8_t *out, int *out_len);
int wpa_process_eapol_m3(wpa_ctx_t *w, const uint8_t *m3, int m3_len, uint8_t *out, int *out_len);
uint16_t wpa_build_rsn_ie(void);
