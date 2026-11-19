#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * hub_uplink -- 허브 ↔ 스마트홈 플랫폼(SmartThings) 통로.
 *
 * 메시지 형식은 통로와 상관없이 같다: [명령 1바이트][내용]. 통로마다 담는 그릇만 다르다.
 *   Zigbee : 전용 클러스터 0xFC31 의 명령 id = 명령, 페이로드 = 내용   (hub_zb_rinnai.c)
 *   LAN    : TCP 한 줄 "<명령 16진 2자리> <내용 16진>\n"               (hub_lan.c)
 *   Matter : (나중)
 * 어느 통로를 쓸지는 비트로 고른다(셋업 화면에서 바꿀 예정, NVS 저장).
 */

#define HUB_UPLINK_ZIGBEE  0x01
#define HUB_UPLINK_LAN     0x02
#define HUB_UPLINK_MATTER  0x04
#define HUB_UPLINK_DEFAULT HUB_UPLINK_ZIGBEE   /* LAN(TCP 8484)은 고를 때만 연다 */

/* 플랫폼 → 허브 */
#define HUB_RK_CMD_REQUEST  0x00    /* 명령 텍스트: "f06 b1=1" / "f09 a0=7,b1=1,d4=10" / "refresh", 여러 개는 ";" */
/* 허브 → 플랫폼 */
#define HUB_RK_CMD_STATE    0x01    /* [종류 6|9][MAC끝3바이트][EPC 70 블롭] */
#define HUB_RK_CMD_LINKS    0x02    /* 연결된 기기 목록: ([종류][MAC끝3바이트]) * n. 목록에 없으면 오프라인 */
#define HUB_RK_CMD_ROOM     0x03    /* 온습도 int16 x2 (°C*100, %*100) */
#define HUB_RK_CMD_GAS      0x04    /* u32 x5 (펄스, 계량기 L|FFFFFFFF, 1시간 L, 24시간 L, 유량 L/h) */
#define HUB_RK_CMD_EXPOSE   0x06    /* u32: 이 통로에 노출할 항목 비트(hub_expose.h) */
#define HUB_RK_CMD_PLUGINS  0x05    /* [켜진 플러그인 비트: 1 온습도계, 2 가스] — 꺼진 것의 기기는 지운다 */
#define HUB_RK_CMD_PAIRS    0x07    /* 보일러↔써머스탯 짝: ([f09 MAC끝3(3)][f06 MAC끝3(3)]) * n (관리 화면 수동) */
#define HUB_RK_CMD_FORGET   0x08    /* 기기 삭제: [종류 6|9][MAC끝3바이트] — 드라이버가 그 기기 자식들을 지운다 */

void hub_uplink_load(void);                    /* NVS 에서 고른 통로 읽기 */
uint8_t hub_uplink_mask(void);
esp_err_t hub_uplink_set_mask(uint8_t mask);   /* 저장(성공해야 바꿈). 바뀌면 관리 화면이 재시작해 적용 */

void hub_uplink_render(char *out, size_t cap);              /* 관리 화면: 연결 방식 고르기 */
bool hub_uplink_apply(const char *form, char *msg, size_t len);  /* POST /admin/transport */

/* 켜진 모든 통로로 보낸다 */
void hub_uplink_send(uint8_t cmd, const uint8_t *data, uint16_t n);
/* 한 통로로만(켜져 있을 때) — 통로마다 내용이 다른 메시지(노출 설정)용 */
void hub_uplink_send_to(uint8_t uplink, uint8_t cmd, const uint8_t *data, uint16_t n);

/* 어느 통로든 플랫폼에서 요청이 오면 */
void hub_uplink_on_request(uint8_t from, const char *text, int len);   /* from = 받은 통로(꺼진 통로면 버림) */
