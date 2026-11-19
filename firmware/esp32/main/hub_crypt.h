#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifndef HUB_CRYPT_SELFTEST
#define HUB_CRYPT_SELFTEST 0   /* 1 = 시험 빌드: USB "crypt selftest" → 다음 부팅에 고정 키로 암호화→검증→평문 복원(eFuse 안 씀) */
#endif

/*
 * hub_crypt -- 저장 데이터(NVS: 앱 설정 'nvs' + Zigbee 가입 정보 'zb_nvs') 암호화.
 *
 * ESP32-C6 의 HMAC 주변장치 방식: eFuse 키 블록(KEY0~KEY5) 하나에 HMAC_UP 용도의 무작위 키를 굽고, 그 키로
 * NVS 의 XTS-AES 키를 칩 안에서 만들어 쓴다. eFuse 키는 굽는 순간 읽기·쓰기 금지가 걸려 USB 로도 꺼낼 수 없다.
 *
 *   - 켜기는 관리 화면에서 키 블록을 골라 요청 → 다음 부팅 때(Wi-Fi·Zigbee 시작 전) 처리:
 *       평문 데이터를 RAM 으로 다 읽음 → (블록이 비었으면) 퓨즈 굽기 → 두 파티션 지우고 암호화로 다시 열기 → 되쓰기 → 재시작
 *   - 부팅할 때 HMAC_UP 용도의 키 블록이 있으면 그 키로 암호화된 저장소를 연다(= 암호화 켜짐). 없으면 평문.
 *   - eFuse 는 되돌릴 수 없으므로 한 번 켜면 끌 수 없다.
 *   - CONFIG_NVS_ENCRYPTION 은 쓰지 않는다(켜면 부팅 때 nvs_flash_init 이 빈 블록에 키를 '저절로' 굽는다).
 */

/* app_main 맨 앞: 두 NVS 파티션을 연다(필요하면 암호화 켜기 처리 후 재시작). zb_reset = zb_nvs 를 지우고 새로 만들었나 */
esp_err_t hub_crypt_boot(bool *zb_reset);

bool hub_crypt_on(void);                 /* 이번 부팅에 암호화된 저장소를 쓰나 */
int hub_crypt_key_block(void);           /* 쓰는 키 블록 0~5, 없으면 -1 */

void hub_crypt_render(char *out, size_t cap);                     /* 관리 화면 '저장 데이터 암호화' */
bool hub_crypt_apply(const char *form, char *msg, size_t msglen);   /* POST /admin/crypt: 켜기 예약 → 재시작 */
void hub_crypt_print(void);                                         /* USB diag: 상태·키 블록 */
