#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "nvs.h"

/*
 * hub_store -- 검사값(CRC32)을 같이 저장하는 NVS 래퍼.
 *
 * NVS 는 항목마다 CRC 가 있지만, 깨진 항목은 조용히 지우고 "없음"으로 돌려줘 앱이 기본값으로 넘어간다
 * (흔적이 안 남음). 그래서 값은 원래 키에, 값의 CRC32 는 "<키>#"(u32)에 따로 둔다.
 *   읽을 때  CRC 있는데 값 없음 → "값 사라짐"   (NVS 가 깨진 항목을 지운 경우)
 *            둘 다 있는데 불일치 → "검사값 불일치" (저장 중 전원 끊김 등) — 값은 그대로 쓰고 CRC 를 맞춰 둔다
 *            CRC 없는 옛 값       → 그대로 받고 CRC 를 붙인다(이 기능 전에 저장된 값)
 * 오류는 hub_store_report(): 로그 · 이벤트(store_bad) · /api/stats · 관리 화면 경고 · 빨간 LED 깜빡임.
 * ns 는 보고에 쓰는 이름(보통 nvs 네임스페이스).
 */
esp_err_t hs_set_str(nvs_handle_t h, const char *key, const char *v);
esp_err_t hs_get_str(nvs_handle_t h, const char *ns, const char *key, char *out, size_t *len);
esp_err_t hs_set_blob(nvs_handle_t h, const char *key, const void *v, size_t n);
esp_err_t hs_get_blob(nvs_handle_t h, const char *ns, const char *key, void *out, size_t *len);
esp_err_t hs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t hs_get_u8(nvs_handle_t h, const char *ns, const char *key, uint8_t *v);
esp_err_t hs_set_u32(nvs_handle_t h, const char *key, uint32_t v);
esp_err_t hs_get_u32(nvs_handle_t h, const char *ns, const char *key, uint32_t *v);
esp_err_t hs_set_i32(nvs_handle_t h, const char *key, int32_t v);
esp_err_t hs_get_i32(nvs_handle_t h, const char *ns, const char *key, int32_t *v);
esp_err_t hs_erase(nvs_handle_t h, const char *key);      /* 값과 검사값 둘 다 */

/* 오류 보고(다른 모듈도 쓸 수 있음 — 예: Zigbee 가입 정보 사라짐) */
void hub_store_report(const char *ns, const char *key, const char *what);
int hub_store_error_count(void);
/* 최근 오류들을 JSON 배열 문자열로(예: ["pairs/p0 값 사라짐"]). 관리 화면·API 용 */
void hub_store_errors_json(char *out, size_t cap);
void hub_store_errors_html(char *out, size_t cap);       /* 관리 화면 경고 한 줄(없으면 빈 문자열) */
