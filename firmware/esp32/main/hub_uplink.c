/*
 * hub_uplink.c -- 통로 고르기 + 보내기/받기 모으기. 설명은 hub_uplink.h.
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "hub_store.h"

#include "hub_uplink.h"
#include "hub_zb_rinnai.h"
#include "hub_lan.h"
#include "hub_rinnai.h"
#include "hub_sensors.h"
#include "hub_expose.h"

void hub_rinnai_resend(void);   /* 연결·상태 다시 보내도록 깨운다(hub_rinnai.c) */

static const char *TAG = "HUB_UPLINK";
static uint8_t s_mask = HUB_UPLINK_DEFAULT;

void hub_uplink_load(void)
{
    nvs_handle_t h;
    if (nvs_open("uplink", NVS_READWRITE, &h) == ESP_OK) {
        hs_get_u8(h, "uplink", "mask", &s_mask);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "통로: Zigbee=%d LAN=%d Matter=%d", !!(s_mask & HUB_UPLINK_ZIGBEE), !!(s_mask & HUB_UPLINK_LAN),
             !!(s_mask & HUB_UPLINK_MATTER));
}

uint8_t hub_uplink_mask(void) { return s_mask; }

void hub_uplink_set_mask(uint8_t mask)
{
    s_mask = mask;
    nvs_handle_t h;
    if (nvs_open("uplink", NVS_READWRITE, &h) == ESP_OK) {
        hs_set_u8(h, "mask", mask);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 관리 화면: 어떤 연결 방식(통로)을 쓸지 고르기. 고른 방식만 노출 설정에 나온다. Matter 는 준비 중. */
void hub_uplink_render(char *o, size_t cap)
{
    static const struct { uint8_t bit; const char *name; bool ready; } CH[] = {
        { HUB_UPLINK_ZIGBEE, "Zigbee", true }, { HUB_UPLINK_LAN, "LAN", true },
        { HUB_UPLINK_MATTER, "Matter", false },
    };
    char t[256];
    strlcat(o, "<h2>연결 방식</h2><div class=card><p class=muted>"
               "허브를 SmartThings 에 어떤 방식으로 연결할지 고릅니다. 여기서 고른 방식만 아래 '노출 설정'에서 "
               "조절할 수 있습니다. Matter 는 준비 중입니다.</p>"
               "<form method=post action=/admin/transport>", cap);
    for (int i = 0; i < 3; i++) {
        snprintf(t, sizeof(t), "<label class=chk><input type=checkbox name=u%d%s%s> %s%s</label>",
                 CH[i].bit, (s_mask & CH[i].bit) ? " checked" : "", CH[i].ready ? "" : " disabled",
                 CH[i].name, CH[i].ready ? "" : " (준비 중)");
        strlcat(o, t, cap);
    }
    strlcat(o, "<button class=sv>연결 방식 저장</button></form></div>", cap);
}

bool hub_uplink_apply(const char *form, char *msg, size_t len)
{
    static const uint8_t AVAIL[] = { HUB_UPLINK_ZIGBEE, HUB_UPLINK_LAN };   /* Matter 제외 */
    char key[8], v[8];
    uint8_t m = 0;
    for (int i = 0; i < (int)(sizeof(AVAIL) / sizeof(AVAIL[0])); i++) {
        snprintf(key, sizeof(key), "u%d", AVAIL[i]);
        if (httpd_query_key_value(form, key, v, sizeof(v)) == ESP_OK) {
            m |= AVAIL[i];
        }
    }
    hub_uplink_set_mask(m);
    hub_rinnai_resend();        /* 켠 통로에 연결·상태 다시 보냄 */
    hub_expose_send_all();      /* 노출 마스크도(끈 통로엔 안 감) */
    snprintf(msg, len, "연결 방식을 저장했습니다 (Zigbee %s · LAN %s)",
             (m & HUB_UPLINK_ZIGBEE) ? "켜짐" : "꺼짐", (m & HUB_UPLINK_LAN) ? "켜짐" : "꺼짐");
    return true;
}

void hub_uplink_send_to(uint8_t uplink, uint8_t cmd, const uint8_t *data, uint16_t n)
{
    if (!(s_mask & uplink)) {
        return;
    }
    if (uplink == HUB_UPLINK_ZIGBEE) {
        hub_zigbee_send_raw(cmd, data, n);
    } else if (uplink == HUB_UPLINK_LAN) {
        hub_lan_send(cmd, data, n);
    }
}

void hub_uplink_send(uint8_t cmd, const uint8_t *data, uint16_t n)
{
    hub_uplink_send_to(HUB_UPLINK_ZIGBEE, cmd, data, n);
    hub_uplink_send_to(HUB_UPLINK_LAN, cmd, data, n);
}

void hub_uplink_on_request(const char *text, int len)
{
    ESP_LOGI(TAG, "요청: %.*s", len, text);
    if (len == 7 && !memcmp(text, "refresh", 7)) {
        hub_sensors_refresh();
    }
    hub_rinnai_command(text, len);
}
