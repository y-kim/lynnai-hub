/*
 * hub_state.c -- 허브 설정 상태(NVS).
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "hub_store.h"

#include "hub_state.h"

static const char *TAG = "HUB_STATE";

typedef struct {
    char sta_ssid[HUB_SSID_MAX + 1], sta_pass[HUB_PASS_MAX + 1];
    char ap_ssid[HUB_SSID_MAX + 1], ap_pass[HUB_PASS_MAX + 1];
} wifi_blob_t;


#define NS             "lynnai"
#define PBKDF2_ITERS   10000
#define SALT_LEN       16
#define HASH_LEN       32

/* 혼동 문자(0/O, 1/I/L) 뺀 32자 — 스티커·화면에서 옮겨 적기 쉬움 */
static const char ALPHABET[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

static void new_device_id(char out[HUB_ID_LEN + 1])
{
    uint32_t r = esp_random();          /* RF 가 켜져 있으면 진짜 HW 난수 */
    for (int i = 0; i < HUB_ID_LEN; i++) {
        out[i] = ALPHABET[r & 0x1F];
        r >>= 5;
    }
    out[HUB_ID_LEN] = '\0';
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    if (hs_get_str(h, NS, key, out, &n) != ESP_OK) {
        out[0] = '\0';
    }
}

esp_err_t hub_state_load(hub_state_t *st)
{
    memset(st, 0, sizeof(*st));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    get_str(h, "did", st->device_id, sizeof(st->device_id));
    if (strlen(st->device_id) != HUB_ID_LEN) {           /* 첫 부팅(또는 공장초기화 직후) */
        new_device_id(st->device_id);
        hs_set_str(h, "did", st->device_id);
        nvs_commit(h);
        ESP_LOGI(TAG, "첫 부팅: 새 기기 ID %s", st->device_id);
    }
    uint8_t prov = 0;
    hs_get_u8(h, NS, "prov", &prov);
    st->provisioned = prov == 1;
    get_str(h, "ap_ssid", st->ap_ssid, sizeof(st->ap_ssid));
    get_str(h, "ap_pass", st->ap_pass, sizeof(st->ap_pass));
    get_str(h, "sta_ssid", st->sta_ssid, sizeof(st->sta_ssid));
    get_str(h, "sta_pass", st->sta_pass, sizeof(st->sta_pass));
    wifi_blob_t wb;                                   /* 관리 화면에서 바꾼 Wi-Fi(한 값으로 저장 — 있으면 이게 먼저) */
    size_t wn = sizeof(wb);
    if (hs_get_blob(h, NS, "wifi4", &wb, &wn) == ESP_OK && wn == sizeof(wb) &&
        wb.sta_ssid[HUB_SSID_MAX] == 0 && wb.sta_pass[HUB_PASS_MAX] == 0 && wb.ap_ssid[HUB_SSID_MAX] == 0 &&
        wb.ap_pass[HUB_PASS_MAX] == 0 && wb.sta_ssid[0] && wb.ap_ssid[0]) {
        strlcpy(st->sta_ssid, wb.sta_ssid, sizeof(st->sta_ssid));
        strlcpy(st->sta_pass, wb.sta_pass, sizeof(st->sta_pass));
        strlcpy(st->ap_ssid, wb.ap_ssid, sizeof(st->ap_ssid));
        strlcpy(st->ap_pass, wb.ap_pass, sizeof(st->ap_pass));
    }
    memset(&wb, 0, sizeof(wb));
    nvs_close(h);
    return ESP_OK;
}

void hub_state_setup_ssid(const hub_state_t *st, char *out, int len)
{
    snprintf(out, len, "LYNNAI-SETUP-%s", st->device_id);
}

void hub_state_default_ap_ssid(const hub_state_t *st, char *out, int len)
{
    snprintf(out, len, "LYNNAI-HUB-%s", st->device_id);
}

static bool has_ctrl(const char *s)
{
    for (; *s; s++) {
        if ((unsigned char)*s < 0x20 || *s == 0x7f) {
            return true;
        }
    }
    return false;
}

static esp_err_t hash_pw(const char *pw, const uint8_t salt[SALT_LEN], uint8_t out[HASH_LEN])
{
    int r = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw, strlen(pw),
                                          salt, SALT_LEN, PBKDF2_ITERS, HASH_LEN, out);
    return r == 0 ? ESP_OK : ESP_FAIL;
}

#define FAIL(msg) do { snprintf(err, err_len, "%s", msg); return ESP_ERR_INVALID_ARG; } while (0)

esp_err_t hub_state_provision(hub_state_t *st,
                              const char *sta_ssid, const char *sta_pass,
                              const char *ap_ssid, const char *ap_pass,
                              const char *admin_pw, char *err, int err_len)
{
    if (st->provisioned) {
        FAIL("이미 설정된 허브입니다");
    }
    size_t sl = strlen(sta_ssid), al = strlen(ap_ssid);
    if (sl < 1 || sl > HUB_SSID_MAX) FAIL("집 Wi-Fi 이름은 1~32자");
    if (strlen(sta_pass) > 63) FAIL("집 Wi-Fi 암호가 너무 깁니다");
    if (al < 1 || al > HUB_SSID_MAX) FAIL("허브 네트워크 이름은 1~32자");
    if (strlen(ap_pass) < 8 || strlen(ap_pass) > 63) FAIL("허브 Wi-Fi 암호는 8~63자");
    if (strlen(admin_pw) < 8) FAIL("관리자 암호는 8자 이상");
    if (has_ctrl(sta_ssid) || has_ctrl(sta_pass) || has_ctrl(ap_ssid) || has_ctrl(ap_pass)) {
        FAIL("이름·암호에 쓸 수 없는 문자가 있습니다");
    }

    uint8_t salt[SALT_LEN], hash[HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    if (hash_pw(admin_pw, salt, hash) != ESP_OK) FAIL("암호 처리 실패");

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) FAIL("저장소 열기 실패");
    /* 필수 항목을 다 쓰고 커밋된 뒤에만 '설정 완료'를 표시한다(일부만 저장된 채 완료로 보이지 않게) */
    esp_err_t e = hs_set_str(h, "sta_ssid", sta_ssid);
    if (e == ESP_OK) e = hs_set_str(h, "sta_pass", sta_pass);
    if (e == ESP_OK) e = hs_set_str(h, "ap_ssid", ap_ssid);
    if (e == ESP_OK) e = hs_set_str(h, "ap_pass", ap_pass);
    if (e == ESP_OK) e = hs_set_blob(h, "adm_salt", salt, sizeof(salt));
    if (e == ESP_OK) e = hs_set_blob(h, "adm_hash", hash, sizeof(hash));
    if (e == ESP_OK) e = nvs_commit(h);
    if (e == ESP_OK) e = hs_set_u8(h, "prov", 1);      /* 마지막에 표시 → 중간에 꺼져도 미설정으로 남음 */
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) FAIL("저장 실패");

    strlcpy(st->sta_ssid, sta_ssid, sizeof(st->sta_ssid));
    strlcpy(st->sta_pass, sta_pass, sizeof(st->sta_pass));
    strlcpy(st->ap_ssid, ap_ssid, sizeof(st->ap_ssid));
    strlcpy(st->ap_pass, ap_pass, sizeof(st->ap_pass));
    st->provisioned = true;
    ESP_LOGI(TAG, "설정 완료: 집 Wi-Fi '%s', 허브 AP '%s'", sta_ssid, ap_ssid);
    return ESP_OK;
}

/* 설정 후 Wi-Fi(집 Wi-Fi·허브 AP) 이름·암호 바꾸기. 호출부가 바로 hub_wifi_apply 로 재적용한다. */
esp_err_t hub_state_set_wifi(hub_state_t *st,
                             const char *sta_ssid, const char *sta_pass,
                             const char *ap_ssid, const char *ap_pass,
                             char *err, int err_len)
{
    size_t sl = strlen(sta_ssid), al = strlen(ap_ssid);
    if (sl < 1 || sl > HUB_SSID_MAX) FAIL("집 Wi-Fi 이름은 1~32자");
    if (strlen(sta_pass) > 63) FAIL("집 Wi-Fi 암호가 너무 깁니다");
    if (al < 1 || al > HUB_SSID_MAX) FAIL("허브 네트워크 이름은 1~32자");
    if (strlen(ap_pass) < 8 || strlen(ap_pass) > 63) FAIL("허브 Wi-Fi 암호는 8~63자");
    if (has_ctrl(sta_ssid) || has_ctrl(sta_pass) || has_ctrl(ap_ssid) || has_ctrl(ap_pass)) {
        FAIL("이름·암호에 쓸 수 없는 문자가 있습니다");
    }

    /* 네 값을 한 값(wifi4)으로 쓴다 — 따로 쓰면 그 사이에 꺼졌을 때 새 이름에 옛 암호가 남아 집 Wi-Fi 에 못 붙었다.
     * 처음 설정 때 쓴 네 값은 그대로 두고, 읽을 때 wifi4 가 있으면 그것을 먼저 본다(관리자 암호 adm_cred 와 같은 방식) */
    wifi_blob_t wb;
    memset(&wb, 0, sizeof(wb));
    strlcpy(wb.sta_ssid, sta_ssid, sizeof(wb.sta_ssid));
    strlcpy(wb.sta_pass, sta_pass, sizeof(wb.sta_pass));
    strlcpy(wb.ap_ssid, ap_ssid, sizeof(wb.ap_ssid));
    strlcpy(wb.ap_pass, ap_pass, sizeof(wb.ap_pass));
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) FAIL("저장소 열기 실패");
    esp_err_t e = hs_set_blob(h, "wifi4", &wb, sizeof(wb));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    memset(&wb, 0, sizeof(wb));
    if (e != ESP_OK) FAIL("저장 실패");

    strlcpy(st->sta_ssid, sta_ssid, sizeof(st->sta_ssid));
    strlcpy(st->sta_pass, sta_pass, sizeof(st->sta_pass));
    strlcpy(st->ap_ssid, ap_ssid, sizeof(st->ap_ssid));
    strlcpy(st->ap_pass, ap_pass, sizeof(st->ap_pass));
    ESP_LOGI(TAG, "Wi-Fi 바꿈: 집 Wi-Fi '%s', 허브 AP '%s'", sta_ssid, ap_ssid);
    return ESP_OK;
}

esp_err_t hub_state_set_admin(const char *admin_pw, char *err, int err_len)
{
    size_t n = strlen(admin_pw);
    if (n < 8) FAIL("새 암호는 8자 이상");
    if (n > HUB_PASS_MAX) FAIL("새 암호가 너무 깁니다");
    if (has_ctrl(admin_pw)) FAIL("암호에 쓸 수 없는 문자가 있습니다");
    uint8_t cred[SALT_LEN + HASH_LEN];
    esp_fill_random(cred, SALT_LEN);
    if (hash_pw(admin_pw, cred, cred + SALT_LEN) != ESP_OK) FAIL("암호 처리 실패");
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) FAIL("저장소 열기 실패");
    /* 소금과 해시를 한 값(adm_cred)으로 쓴다 — 따로 쓰면 그 사이에 꺼졌을 때 짝이 안 맞아 어느 암호로도 못 들어온다.
     * 처음 설정 때 쓴 옛 두 값(adm_salt·adm_hash)은 그대로 두고, 확인할 때 adm_cred 가 있으면 그것을 먼저 본다. */
    esp_err_t e = hs_set_blob(h, "adm_cred", cred, sizeof(cred));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    memset(cred, 0, sizeof(cred));
    if (e != ESP_OK) FAIL("저장 실패 — 암호는 바뀌지 않았습니다");
    ESP_LOGW(TAG, "관리자 암호가 바뀜");
    return ESP_OK;
}

bool hub_state_verify_admin(const char *pw)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t salt[SALT_LEN], want[HASH_LEN], got[HASH_LEN], cred[SALT_LEN + HASH_LEN];
    size_t sl = sizeof(salt), hl = sizeof(want), cl = sizeof(cred);
    bool ok;
    if (hs_get_blob(h, NS, "adm_cred", cred, &cl) == ESP_OK && cl == sizeof(cred)) {   /* 암호를 바꾼 적이 있다 */
        memcpy(salt, cred, SALT_LEN);
        memcpy(want, cred + SALT_LEN, HASH_LEN);
        ok = hash_pw(pw, salt, got) == ESP_OK;
    } else {
        ok = hs_get_blob(h, NS, "adm_salt", salt, &sl) == ESP_OK &&
             hs_get_blob(h, NS, "adm_hash", want, &hl) == ESP_OK &&
             sl == SALT_LEN && hl == HASH_LEN && hash_pw(pw, salt, got) == ESP_OK;
    }
    nvs_close(h);
    if (!ok) {
        return false;
    }
    uint8_t diff = 0;                               /* 상수시간 비교 */
    for (int i = 0; i < HASH_LEN; i++) {
        diff |= got[i] ^ want[i];
    }
    return diff == 0;
}
