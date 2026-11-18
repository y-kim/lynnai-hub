#pragma once
#include "ezbee/zcl/zcl_desc.h"

#define HUB_RK_CLUSTER_ID   0xFC31
#define HUB_RK_CMD_REQUEST  0x00    /* ST → 허브: 명령 텍스트 */
#define HUB_RK_CMD_STATE    0x01    /* 허브 → ST: [종류][블롭] */
#define HUB_RK_CMD_LINKS    0x02    /* 허브 → ST: [연결 비트] */

/* 허브 엔드포인트에 붙일 클러스터 */
ezb_zcl_cluster_desc_t hub_zb_rinnai_cluster_desc(void);
