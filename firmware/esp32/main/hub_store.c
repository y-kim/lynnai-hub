/*
 * hub_store.c -- 검사값(CRC32)을 같이 저장하는 NVS 래퍼. 설명은 hub_store.h.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"

#include "hub_store.h"
#include "hub_events.h"
#include "hub_led.h"

static const char *TAG = "HUB_STORE";

/* ---------------------------------------------------------------- 오류 보고 */

#define MAXR 4
#define RLEN 56
static char s_err[MAXR][RLEN];
static int s_nerr;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void hub_store_report(const char *ns, const char *key, const char *what)
{
    char line[RLEN];
    snprintf(line, sizeof(line), "%s/%s %s", ns ? ns : "?", key ? key : "?", what);
    ESP_LOGE(TAG, "저장 데이터 오류: %s — USB 연결해 점검", line);
    portENTER_CRITICAL(&s_mux);
    memcpy(s_err[s_nerr % MAXR], line, RLEN);
    s_nerr++;
    portEXIT_CRITICAL(&s_mux);
    hub_event(EV_STORE_BAD, NULL, s_nerr, 0);
    hub_led_alarm();
}

int hub_store_error_count(void) { return s_nerr; }

/* 최근 것부터 */
static void each(void (*fn)(const char *line, void *arg), void *arg)
{
    char snap[MAXR][RLEN];
    int n;
    portENTER_CRITICAL(&s_mux);
    n = s_nerr;
    memcpy(snap, s_err, sizeof(snap));
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < n && i < MAXR; i++) {
        fn(snap[(n - 1 - i) % MAXR], arg);
    }
}

typedef struct { char *o; size_t cap; int k; } acc_t;

static void json_one(const char *line, void *arg)
{
    acc_t *a = arg;
    strlcat(a->o, a->k++ ? ",\"" : "\"", a->cap);
    strlcat(a->o, line, a->cap);
    strlcat(a->o, "\"", a->cap);
}

void hub_store_errors_json(char *out, size_t cap)
{
    acc_t a = { out, cap, 0 };
    out[0] = 0;
    strlcat(out, "[", cap);
    each(json_one, &a);
    strlcat(out, "]", cap);
}

static void html_one(const char *line, void *arg)
{
    acc_t *a = arg;
    strlcat(a->o, a->k++ ? ", " : "", a->cap);
    strlcat(a->o, line, a->cap);
}

void hub_store_errors_html(char *out, size_t cap)
{
    out[0] = 0;
    if (!s_nerr) {
        return;
    }
    char n[24];
    snprintf(n, sizeof(n), "%d", s_nerr);
    strlcat(out, "<div class=card><p class=warn><b>저장 데이터 오류 ", cap);
    strlcat(out, n, cap);
    strlcat(out, "건</b> — 빨간 LED 가 깜빡입니다. USB 를 연결해 점검하세요.<br><small>", cap);
    acc_t a = { out, cap, 0 };
    each(html_one, &a);
    strlcat(out, "</small></p></div>", cap);
}

/* ---------------------------------------------------------------- 검사값 */

static void ckey(char out[NVS_KEY_NAME_MAX_SIZE], const char *key)
{
    snprintf(out, NVS_KEY_NAME_MAX_SIZE, "%.*s#", NVS_KEY_NAME_MAX_SIZE - 2, key);
}

static uint32_t crc(const void *d, size_t n) { return esp_rom_crc32_le(0, d, n); }

static void put_crc(nvs_handle_t h, const char *key, uint32_t c)
{
    char ck[NVS_KEY_NAME_MAX_SIZE];
    ckey(ck, key);
    if (nvs_set_u32(h, ck, c) == ESP_OK) {   /* 읽기 전용 핸들이면 실패 — 다음 저장 때 붙는다 */
        nvs_commit(h);
    }
}

/* 값 읽기 결과(vr)와 실제 값 바이트로 검사. 반환은 vr 그대로(사라졌으면 NOT_FOUND → 호출자 기본값). */
static esp_err_t check(nvs_handle_t h, const char *ns, const char *key, esp_err_t vr, const void *d, size_t n)
{
    char ck[NVS_KEY_NAME_MAX_SIZE];
    ckey(ck, key);
    uint32_t want = 0;
    esp_err_t cr = nvs_get_u32(h, ck, &want);
    if (vr == ESP_OK) {
        uint32_t got = crc(d, n);
        if (cr == ESP_ERR_NVS_NOT_FOUND) {
            put_crc(h, key, got);                         /* 이 기능 전에 저장된 값: 검사값 붙임 */
        } else if (cr == ESP_OK && want != got) {
            hub_store_report(ns, key, "검사값 불일치");
            put_crc(h, key, got);                         /* 한 번 보고하고 맞춰 둠 */
        }
        return ESP_OK;
    }
    if (vr == ESP_ERR_NVS_NOT_FOUND && cr == ESP_OK) {
        hub_store_report(ns, key, "값 사라짐");           /* 검사값만 남음 = 값이 깨져 지워짐 */
    }
    return vr;
}

esp_err_t hs_set_str(nvs_handle_t h, const char *key, const char *v)
{
    esp_err_t e = nvs_set_str(h, key, v);
    if (e == ESP_OK) {
        put_crc(h, key, crc(v, strlen(v)));
    }
    return e;
}

esp_err_t hs_get_str(nvs_handle_t h, const char *ns, const char *key, char *out, size_t *len)
{
    esp_err_t e = nvs_get_str(h, key, out, len);
    return check(h, ns, key, e, out, e == ESP_OK ? strlen(out) : 0);
}

esp_err_t hs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n)
{
    esp_err_t e = nvs_set_blob(h, key, v, n);
    if (e == ESP_OK) {
        put_crc(h, key, crc(v, n));
    }
    return e;
}

esp_err_t hs_get_blob(nvs_handle_t h, const char *ns, const char *key, void *out, size_t *len)
{
    esp_err_t e = nvs_get_blob(h, key, out, len);
    return check(h, ns, key, e, out, e == ESP_OK ? *len : 0);
}

#define SCALAR(T, NV)                                                                       \
    esp_err_t hs_set_##NV(nvs_handle_t h, const char *key, T v)                            \
    {                                                                                       \
        esp_err_t e = nvs_set_##NV(h, key, v);                                              \
        if (e == ESP_OK) {                                                                  \
            put_crc(h, key, crc(&v, sizeof(v)));                                            \
        }                                                                                   \
        return e;                                                                           \
    }                                                                                       \
    esp_err_t hs_get_##NV(nvs_handle_t h, const char *ns, const char *key, T *v)           \
    {                                                                                       \
        T tmp;                                                                              \
        esp_err_t e = nvs_get_##NV(h, key, &tmp);                                           \
        e = check(h, ns, key, e, &tmp, sizeof(tmp));                                        \
        if (e == ESP_OK) {                                                                  \
            *v = tmp;                                                                       \
        }                                                                                   \
        return e;                                                                           \
    }

SCALAR(uint8_t, u8)
SCALAR(uint32_t, u32)
SCALAR(int32_t, i32)

esp_err_t hs_erase(nvs_handle_t h, const char *key)
{
    char ck[NVS_KEY_NAME_MAX_SIZE];
    ckey(ck, key);
    nvs_erase_key(h, ck);
    return nvs_erase_key(h, key);
}
