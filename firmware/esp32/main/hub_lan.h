#pragma once
#include <stdbool.h>
#include <stdint.h>

/* LAN 통로(SSDP 찾기 + TCP 8484 줄 단위). 형식은 hub_lan.c 맨 위. */
void hub_lan_start(const char *device_id);
void hub_lan_send(uint8_t cmd, const uint8_t *data, uint16_t n);
void hub_lan_pairing_open(int seconds);      /* 새 드라이버/위성 허브 등록 허용(버튼 짧게) */
void hub_lan_token_remove(int slot);         /* 그 토큰 칸을 비운다(위성 삭제 때) */

/* 멀티 허브 "위성" 다이얼러(전화 거는 쪽). 상태는 관리화면용. */
void hub_lan_guest_reconnect(void);          /* 설정 바뀜/수동 → 중심 허브로 다시 붙기 */
void hub_lan_guest_repair(void);             /* 사람이 '중심 허브에 연결하기'를 누름: 다시 페어링 허용 + 다시 연결 */
bool hub_lan_guest_online(void);
const char *hub_lan_guest_status(void);
