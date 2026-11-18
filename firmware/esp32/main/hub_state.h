#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* 허브 설정 상태(NVS 네임스페이스 "lynnai"). Pi 판 appliance/core/state.py 와 같은 모델.
 *  - device_id : 첫 부팅 때 HW 난수로 6글자. 공장초기화하면 새로 뽑힌다(양도 후 추적 불가).
 *  - 미설정이면 개방 설정 AP(LYNNAI-SETUP-<id>), 설정 후엔 집 Wi-Fi(STA) + WPA2 허브 AP.
 *  - 관리자 암호는 PBKDF2-HMAC-SHA256 해시만 저장. */

#define HUB_ID_LEN        6
#define HUB_SSID_MAX      32
#define HUB_PASS_MAX      64

typedef struct {
    char device_id[HUB_ID_LEN + 1];
    bool provisioned;
    char ap_ssid[HUB_SSID_MAX + 1];     /* 허브 전용 AP (설정 후) */
    char ap_pass[HUB_PASS_MAX + 1];
    char sta_ssid[HUB_SSID_MAX + 1];    /* 집 Wi-Fi */
    char sta_pass[HUB_PASS_MAX + 1];
} hub_state_t;

/* NVS 에서 읽는다. 없으면(첫 부팅) 기기 ID 를 새로 뽑아 저장한다. */
esp_err_t hub_state_load(hub_state_t *st);

/* 설정용 AP 이름(LYNNAI-SETUP-<id>)과 허브 AP 기본 이름(LYNNAI-HUB-<id>) */
void hub_state_setup_ssid(const hub_state_t *st, char *out, int len);
void hub_state_default_ap_ssid(const hub_state_t *st, char *out, int len);

/* 첫 설정을 한 번에 확정한다. 실패 시 err 에 한국어 사유를 넣고 아무것도 저장하지 않는다. */
esp_err_t hub_state_provision(hub_state_t *st,
                              const char *sta_ssid, const char *sta_pass,
                              const char *ap_ssid, const char *ap_pass,
                              const char *admin_pw, char *err, int err_len);

bool hub_state_verify_admin(const char *pw);

/* 우리 설정만 지운다(다음 부팅에 새 기기 ID). */
esp_err_t hub_state_factory_reset(void);
