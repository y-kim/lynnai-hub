#pragma once
#include <stdint.h>
#include "ezbee/zcl/zcl_desc.h"
#include "hub_uplink.h"      /* 명령 번호 */

#define HUB_RK_CLUSTER_ID   0xFC31

/* 허브 엔드포인트에 붙일 클러스터 */
ezb_zcl_cluster_desc_t hub_zb_rinnai_cluster_desc(void);

/* 아무 명령이나(가입돼 있을 때만) */
void hub_zigbee_send_raw(uint8_t cmd_id, const uint8_t *data, uint16_t n);
bool hub_zigbee_is_ready(void);     /* ezb_* 를 불러도 되나 */
void hub_zigbee_mark_ready(void);   /* 스택 초기화 후 호출: 그 전엔 전송을 무시 */
