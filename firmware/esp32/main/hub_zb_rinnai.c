/*
 * hub_zb_rinnai.c -- 보일러·각방 상태/명령을 실어 나르는 Zigbee 전용 클러스터(0xFC31).
 *
 *   허브 → SmartThings (서버→클라이언트 명령, 허브 주소 0x0000 / 엔드포인트 1)
 *     0x01 상태:  [종류 6|9][EPC 70 블롭 바이트...]
 *     0x02 연결:  [비트 0 = 보일러 연결됨, 비트 1 = 각방 연결됨]
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

#include "temperature_sensor.h"
#include "hub_rinnai.h"
#include "hub_zb_rinnai.h"

static const char *TAG = "HUB_ZB_RK";

static ezb_zcl_status_t cmd_handler(const ezb_zcl_cmd_hdr_t *h, const uint8_t *payload, uint16_t len)
{
    if (h->cluster_id != HUB_RK_CLUSTER_ID || EZB_ZCL_CMD_FC_IS_TO_CLI_DIRECTION(h->fc)) {
        return EZB_ZCL_STATUS_UNSUP_CMD;
    }
    if (h->cmd_id == HUB_RK_CMD_REQUEST) {
        ESP_LOGI(TAG, "SmartThings 명령: %.*s", len, (const char *)payload);
        hub_rinnai_command((const char *)payload, len);
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

static void send_cmd(uint8_t cmd_id, uint8_t *data, uint16_t n)
{
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
        .data = data,
    };
    ezb_err_t e = ezb_zcl_custom_cluster_cmd_req(&req);
    esp_zigbee_lock_release();
    if (e != EZB_ERR_NONE) {
        ESP_LOGW(TAG, "Zigbee 전송 실패 cmd 0x%02x (%d바이트): %d", cmd_id, n, e);
    }
}

void hub_zigbee_send_state(uint8_t kind, const uint8_t *blob, int n)
{
    uint8_t buf[1 + 64];
    if (n <= 0 || n > 64) {
        return;
    }
    buf[0] = kind;
    memcpy(buf + 1, blob, n);
    ESP_LOGI(TAG, "→ SmartThings f0%d 상태 %d바이트", kind, n);
    send_cmd(HUB_RK_CMD_STATE, buf, n + 1);
}

void hub_zigbee_send_links(uint8_t mask)
{
    ESP_LOGI(TAG, "→ SmartThings 연결 상태 보일러=%d 각방=%d", mask & 1, (mask >> 1) & 1);
    send_cmd(HUB_RK_CMD_LINKS, &mask, 1);
}
