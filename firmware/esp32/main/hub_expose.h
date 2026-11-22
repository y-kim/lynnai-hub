#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "hub_rinnai.h"

/*
 * hub_expose -- 무엇을 어느 플랫폼 통로(Zigbee / LAN / Matter)에 노출할지. 다 끄면 숨김(허브 안에서는 계속 동작).
 *
 * 두 층으로 고른다(관리 화면).
 *   종류별 컨트롤(노출 설정): 통로마다 u32 마스크. 보일러 조절기 컨트롤 / 방 컨트롤은 그 종류 모든 기기에 같이 적용.
 *   기기별 노출(연결된 기기 아래): 통로마다 숨긴 기기 목록 — 기기(MAC끝3)마다 숨김 비트
 *       bit0 = 기기 자신(보일러 조절기), bit1~7 = 온도 조절기의 방 슬롯 z1~z7
 *   플러그인 기기(온습도계·가스)는 마스크의 29·30 비트(노출 설정의 '센서' 묶음 — 설치된 것만 나온다).
 *
 * 전송(HUB_RK_CMD_EXPOSE, 통로마다 자기 것): [u32 마스크 LE] + ([종류][MAC끝3(3)][숨김 비트]) * n
 * 마스크 비트(드라이버 core.lua 의 BIT 표와 같아야 한다):
 *   1 전원  2 난방 모드  3 난방 방식  4 난방 온도  5 온수  6 온수 온도  7 온수 예열      (보일러 조절기)
 *   8 방 켜기/끄기  9 방 설정 온도  10 방 외출  12 방 예약(반복예약)                    (온도 조절기의 방)
 *   11 허브 칩 온도(Zigbee 허브 기기)
 *   29 온습도계 기기  30 가스 계량기 기기
 */

#define EXPOSE_CH_ZIGBEE 0
#define EXPOSE_CH_LAN    1
#define EXPOSE_CH_MATTER 2
#define EXPOSE_NCH       3

#define EXPOSE_HUB_TEMP  11      /* Zigbee 허브 기기 자체의 온도(ESP32 칩 온도 — 방 온도와 다름) */
#define EXPOSE_ROOM_DEV  29
#define EXPOSE_GAS_DEV   30

/* 온도 조절기의 방 슬롯(화면 순서). stat = EPC70 블롭 안 위치, z = 드라이버 존 번호(숨김 비트 위치) */
typedef struct {
    const char *name;
    uint8_t stat;
    uint8_t z;
} hub_room_t;
#define HUB_NROOMS 7
extern const hub_room_t HUB_ROOMS[HUB_NROOMS];
bool hub_room_present(const hub_dev_t *d, int i);   /* 온도 조절기가 이 방을 리포트하나(stat·현재온도 둘 다 0 이면 없음) */

void hub_expose_load(void);
bool hub_expose_bit_on(int ch, int bit);         /* 그 통로 마스크의 비트(예: 허브 칩 온도를 보고할지) */
void hub_expose_send_all(void);                 /* 각 통로에 자기 마스크 + 숨김 목록 */
void hub_expose_prepare(void);                  /* 화면 그리기 전: 옛 설정을 새로 보인 기기에 옮김 */
void hub_expose_forget(uint8_t kind, const uint8_t mac3[3]);   /* 기기 삭제 시 그 기기 항목 정리 */

/* 관리 화면 */
void hub_expose_render_types(char *out, size_t cap);           /* '노출 설정': 종류별 컨트롤 표(폼 포함) */
bool hub_expose_apply(const char *form, char *err, size_t errlen);   /* POST /admin/expose */
/* 연결된 기기 행 아래: 한 기기(sub=0)/방(sub=z) 의 통로 체크박스(폼은 호출부). 이름 h<종류>_<mac>_<sub>_<통로> */
void hub_expose_render_dev(char *out, size_t cap, uint8_t kind, const uint8_t mac3[3], int sub);
void hub_expose_render_dev_marker(char *out, size_t cap, uint8_t kind, const uint8_t mac3[3], uint8_t subs);
bool hub_expose_apply_devices(const char *form);   /* 저장 실패면 false */                /* 연결된 기기 폼 저장 때 같이 */
