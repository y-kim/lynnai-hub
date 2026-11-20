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
 *   - 켜기는 관리 화면에서 키 블록을 골라 요청 → 60초 안에 허브 버튼(짧게)으로 확인 → 재시작해(Wi-Fi·Zigbee 전) 처리:
 *       ① 두 파티션을 원본 그대로 전용 영역(cryptj)에 백업·CRC 확인  ② 항목을 RAM 으로 다 읽음
 *       ③ (블록이 비었으면) 퓨즈 굽기  ④ 암호화로 되쓰기 → 다시 읽어 하나하나 비교
 *       ⑤ 완료 기록 → 영구 마커(cryptj 끝 두 섹터) → 평문 백업 지움 → 재시작
 *     중간에 전원이 끊기면 다음 부팅이 백업을 되돌리고 다시 시도한다(3번까지, 넘으면 평문 그대로 포기).
 *     되돌리기 전에 백업의 CRC 를 먼저 보고, 포기할 때는 '되돌림 끝'을 적은 뒤 백업을 지운다(지우다 꺼져도 다시 덮어쓰지 않게).
 *     전환 기록을 못 읽거나 깨졌으면 판단하지 않고 백업을 남긴 채 복구 모드.
 *   - 부팅 때 마커가 있어야 암호화로 연다. HMAC_UP 키만 있고 마커가 없으면 평문으로 본다(다른 용도의 키와 구분).
 *     단, 저장소가 평문이 아니고 그 키로 풀리면(마커 이전 펌웨어가 암호화한 허브) 마커를 남기고 암호화로 연다.
 *   - 앱 nvs 를 열 수 없으면(가득 참·버전 바뀜) 지우지 않고 복구 모드: LED 4번 + USB 에서 "nvs erase"/"reboot".
 *   - eFuse 는 되돌릴 수 없으므로 한 번 켜면 끌 수 없다.
 *   - CONFIG_NVS_ENCRYPTION 은 쓰지 않는다(켜면 부팅 때 nvs_flash_init 이 빈 블록에 키를 '저절로' 굽는다).
 */

/* app_main 맨 앞: 두 NVS 파티션을 연다(필요하면 암호화 켜기 처리 후 재시작). zb_reset = zb_nvs 를 지우고 새로 만들었나 */
esp_err_t hub_crypt_boot(bool *zb_reset);

bool hub_crypt_on(void);                 /* 이번 부팅에 암호화된 저장소를 쓰나 */
int hub_crypt_key_block(void);           /* 쓰는 키 블록 0~5, 없으면 -1 */

void hub_crypt_render(char *out, size_t cap);                     /* 관리 화면 '저장 데이터 암호화' */
bool hub_crypt_apply(const char *form, char *msg, size_t msglen);   /* POST /admin/crypt: 켜기 준비(60초 안에 버튼) */
bool hub_crypt_button(void);   /* 버튼 짧게: 켜기를 기다리는 중이면 확정·재시작하고 true(그러면 다른 버튼 동작은 안 함) */
void hub_crypt_print(void);                                         /* USB diag: 상태·키 블록 */
