/*
 * hub_zb_rinnai.c -- 보일러·각방 상태/명령을 실어 나르는 Zigbee 전용 클러스터(0xFC31).
 *
 * 메시지 번호·내용은 hub_uplink.h (LAN 과 같다). 여기는 Zigbee 그릇만.
 *   허브 → SmartThings (서버→클라이언트 명령, 허브 주소 0x0000 / 엔드포인트 1)
 *     0x01 상태:  [종류 6|9][EPC 70 블롭 바이트...]
 *     0x02 연결:  [비트 0 = 보일러 연결됨, 비트 1 = 각방 연결됨]
 *     0x03 온습도, 0x04 가스 계량기 (hub_sensors.c)
 *   SmartThings → 허브 (클라이언트→서버)
 *     0x00 명령:  ASCII "f06 b1=1" / "f09 a0=7,b1=1,d4=16" / "refresh", 여러 개는 ";" 로
 */
#include <string.h>
#include "esp_log.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "ezbee/core.h"
#include "ezbee/bdb.h"
#include "ezbee/zcl/cluster/custom.h"

#include "hub_main.h"
#include "hub_zb_rinnai.h"

static const char *TAG = "HUB_ZB_RK";

static ezb_zcl_status_t cmd_handler(const ezb_zcl_cmd_hdr_t *h, const uint8_t *payload, uint16_t len)
{
    if (h->cluster_id != HUB_RK_CLUSTER_ID || EZB_ZCL_CMD_FC_IS_TO_CLI_DIRECTION(h->fc)) {
        return EZB_ZCL_STATUS_UNSUP_CMD;
    }
    if (h->cmd_id == HUB_RK_CMD_REQUEST) {
        /* 명령은 SmartThings 허브(코디네이터, 0x0000)에서만 받는다 — 같은 망의 다른 Zigbee 기기가 보낸 것은 버림 */
        if (h->src_addr.addr_mode != EZB_ADDR_MODE_SHORT || h->src_addr.u.short_addr != 0x0000) {
            ESP_LOGW(TAG, "코디네이터가 아닌 곳(모드 %d, 0x%04x)의 명령 버림", h->src_addr.addr_mode,
                     h->src_addr.addr_mode == EZB_ADDR_MODE_SHORT ? h->src_addr.u.short_addr : 0xffff);
            return EZB_ZCL_STATUS_NOT_AUTHORIZED;
        }
        hub_uplink_on_request(HUB_UPLINK_ZIGBEE, (const char *)payload, len);
        return EZB_ZCL_STATUS_SUCCESS;
    }
    return EZB_ZCL_STATUS_UNSUP_CMD;
}

static uint8_t disc_handler(bool is_recv, uint8_t **list)
{
    static uint8_t recv[] = { HUB_RK_CMD_REQUEST };
    static uint8_t send[] = { HUB_RK_CMD_STATE, HUB_RK_CMD_LINKS };
    *list = is_recv ? recv : send;
    return is_recv ? sizeof(recv) : sizeof(send);
}

static void cluster_init(uint8_t ep_id)
{
    ezb_zcl_custom_cluster_handlers_t h = {
        .cluster_id = HUB_RK_CLUSTER_ID,
        .cluster_role = EZB_ZCL_CLUSTER_SERVER,
        .process_cmd_cb = cmd_handler,
        .cmd_disc_cb = disc_handler,
    };
    ezb_zcl_custom_cluster_handlers_register(&h);
}

static void cluster_deinit(uint8_t ep_id) { (void)ep_id; }

ezb_zcl_cluster_desc_t hub_zb_rinnai_cluster_desc(void)
{
    ezb_zcl_custom_cluster_config_t cfg = {
        .cluster_id = HUB_RK_CLUSTER_ID,
        .init_func = cluster_init,
        .deinit_func = cluster_deinit,
    };
    ezb_zcl_cluster_desc_t d = ezb_zcl_custom_create_cluster_desc(&cfg, EZB_ZCL_CLUSTER_SERVER);
    static const uint16_t version = 1;
    ezb_zcl_custom_cluster_desc_add_attr(d, 0x0000, EZB_ZCL_ATTR_TYPE_UINT16, EZB_ZCL_ATTR_ACCESS_READ, &version);
    return d;
}

static volatile bool s_zb_ready;    /* Zigbee 스택 초기화 전엔 ezb_* 호출 금지(부팅 극초기 센서 전송 대비) */

void hub_zigbee_mark_ready(void) { s_zb_ready = true; }
bool hub_zigbee_is_ready(void) { return s_zb_ready; }

void hub_zigbee_send_raw(uint8_t cmd_id, const uint8_t *data, uint16_t n)
{
    if (!s_zb_ready) {
        return;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    if (!ezb_bdb_dev_joined()) {
        esp_zigbee_lock_release();
        return;
    }
    ezb_zcl_custom_cluster_cmd_t req = {
        .cmd_ctrl = {
            .dst_addr = { .addr_mode = EZB_ADDR_MODE_SHORT, .u.short_addr = 0x0000 },
            .dst_ep = 1,
            .src_ep = ESP_ZIGBEE_HA_TEMPERATURE_SENSOR_EP_ID,
            .cluster_id = HUB_RK_CLUSTER_ID,
            .fc = { .manuf_specific = 0, .direction = EZB_ZCL_CMD_DIRECTION_TO_CLI, .dis_default_rsp = 1 },
        },
        .cmd_id = cmd_id,
        .data_length = n,
        .data = (uint8_t *)data,
    };
    ezb_err_t e = ezb_zcl_custom_cluster_cmd_req(&req);
    esp_zigbee_lock_release();
    if (e != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Zigbee 전송 실패 cmd 0x%02x (%d바이트): %d", cmd_id, n, e);
    }
}
