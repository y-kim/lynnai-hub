#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_http_server.h"

/*
 * hub_ota -- WiFi 로 펌웨어 갱신(OTA) + 원격 재부팅.
 *
 * 파티션은 A/B 백업이 아니라 "라디오 변종 2칸"이다(ota_0=내장, ota_1=외부, hub_radio).
 * 그래서 업로드한 이미지의 변종을 보고 다르게 처리한다:
 *   - 지금 돌고 있는 것과 같은 변종 → 반대 칸에 쓰고 부팅 칸을 바꿔 재시작(구버전은 되돌림용으로 남음).
 *   - 다른(반대 라디오용) 변종      → 그 칸만 갱신하고 부팅은 그대로(라디오를 바꾸면 적용).
 * 이미지 끝의 LYNR 표시로 변종을 읽고, esp_ota_end 가 체크섬/서명을 검증한다.
 *
 * 인증은 포털(관리자 세션 또는 API 토큰)에서 걸고, 여기선 몸통(펌웨어 바이트)만 받는다.
 */

/* HTTP 몸통(펌웨어 .bin 원본)을 받아 반대 칸에 쓴다. 성공 1 / 실패 0, 사람이 읽을 결과를 msg 에.
 * 성공이고 지금과 같은 변종이면 *do_reboot=true (호출 쪽이 응답을 보낸 뒤 재부팅 예약). */
int  hub_ota_write_req(httpd_req_t *req, char *msg, size_t msglen, bool *do_reboot);

/* ---- 자가 되돌리기(시험 부팅) ----
 * USB 없이 OTA 만으로 운영하므로, 새 펌웨어가 제대로 뜨지 못하면 스스로 이전 칸으로 돌아가야 한다
 * (부트로더 롤백은 꺼져 있고 USB 로만 켤 수 있다). OTA 로 부팅 칸을 바꿀 때 '시험 중' 표시를 남기고:
 *   - 새 펌웨어가 부팅 45초 뒤 집 Wi-Fi 에 붙어 있으면 확정(표시 지움).
 *   - 5분 안에 확정하지 못하면 재시작. 확정 못 한 부팅이 3번이면 4번째 부팅에서 이전 칸으로 되돌린다
 *     (계속 죽어 재부팅하는 경우도 같은 셈법에 걸린다).
 * 이 검사는 새 펌웨어 자신이 app_main 초반에 한다 — 그 전에 죽는 펌웨어는 막지 못한다. */
void hub_ota_boot_guard(void);        /* app_main: 저장소를 연 직후 */
void hub_ota_planned_reboot(void);    /* 일부러 하는 재시작 직전에: 이번 부팅을 실패로 세지 않게 */
void hub_ota_state(int *trial, bool *rolled_back);   /* trial: 0 = 시험 아님, n = n번째 시험 부팅 */

/* 응답을 보낸 뒤 잠깐 있다 재시작(관리 화면 재부팅 버튼 / OTA 적용 공용). */
void hub_ota_schedule_reboot(void);

/* 관리 화면 '시스템' 카드(펌웨어 OTA · 재부팅 · Zigbee 상태 한 줄). */
void hub_ota_render(char *out, size_t cap);
