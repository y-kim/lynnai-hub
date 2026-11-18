#pragma once
#include <stdint.h>
#include "ezbee/zcl/zcl_desc.h"

#define HUB_RK_CLUSTER_ID   0xFC31
#define HUB_RK_CMD_REQUEST  0x00    /* ST → 허브: 명령 텍스트 */
#define HUB_RK_CMD_STATE    0x01    /* 허브 → ST: [종류][블롭] */
#define HUB_RK_CMD_LINKS    0x02    /* 허브 → ST: [연결 비트] */
#define HUB_RK_CMD_ROOM     0x03    /* 허브 → ST: 온습도 int16 x2 (°C*100, %*100) */
#define HUB_RK_CMD_GAS      0x04    /* 허브 → ST: u32 x5 (펄스, 계량기 L|FFFFFFFF, 1시간 L, 24시간 L, 유량 L/h) */

/* 허브 엔드포인트에 붙일 클러스터 */
ezb_zcl_cluster_desc_t hub_zb_rinnai_cluster_desc(void);

/* 아무 명령이나(가입돼 있을 때만) */
void hub_zigbee_send_raw(uint8_t cmd_id, const uint8_t *data, uint16_t n);
