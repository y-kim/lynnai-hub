#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * hub_expose -- 무엇을 어느 플랫폼 통로(Zigbee / LAN / Matter)에 노출할지. 관리 화면에서 항목마다 고른다.
 * 통로마다 32비트 마스크 하나(NVS "expose"). 다 끄면 그 항목은 숨김(허브 안에서는 계속 동작).
 * 바뀌면 곧바로 각 통로에 자기 마스크를 보낸다(HUB_RK_CMD_EXPOSE) — 드라이버가 컨트롤을 숨기거나 기기를 빼고 넣는다.
 *
 * 비트(드라이버 core.lua 의 EXPOSE 표와 같아야 한다):
 *   0 보일러 기기   1 전원   2 난방 모드   3 난방 방식   4 난방 온도   5 온수   6 온수 온도   7 온수 예열
 *   8~10 방1(기기/켜기/온도)  11~13 방2  14~16 거실  17~19 방3  20~22 방4  23~25 방5  26~28 방6
 *   29 온습도계 기기   30 가스 계량기 기기   (방은 기기가 리포트하는 슬롯만 실제로 생긴다)
 */

#define EXPOSE_CH_ZIGBEE 0
#define EXPOSE_CH_LAN    1
#define EXPOSE_CH_MATTER 2
#define EXPOSE_NCH       3

#define EXPOSE_ROOM_DEV  29
#define EXPOSE_GAS_DEV   30

void hub_expose_load(void);
uint32_t hub_expose_mask(int ch);
void hub_expose_send_all(void);                 /* 각 통로에 자기 마스크 */

/* 관리 화면 */
void hub_expose_render_rinnai(char *out, size_t cap);              /* 린나이 항목 표 */
void hub_expose_render_bit(char *out, size_t cap, int bit);         /* 플러그인 카드 안: 한 줄 체크박스 */
bool hub_expose_apply(const char *form, char *err, size_t errlen);  /* POST /admin/expose */
