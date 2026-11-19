/*
 * hub_uplink.c -- 통로 고르기 + 보내기/받기 모으기. 설명은 hub_uplink.h.
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

esp_err_t hub_uplink_set_mask(uint8_t mask)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("uplink", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = hs_set_u8(h, "mask", mask);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e == ESP_OK) {
        s_mask = mask;
    }
    return e;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));                 /* 응답을 먼저 보내고 */
    esp_restart();
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
    strlcat(o, "<button class=sv>연결 방식 저장</button></form>", cap);
    if (s_mask & HUB_UPLINK_ZIGBEE) {                 /* 허브 버튼 짧게와 같음 — Wi-Fi 가 최대 3분 멈춘다 */
        strlcat(o, "<form method=post action=/api/zigbee/join><p class=muted>SmartThings 앱에서 '기기 추가 → 주변 기기 "
                   "검색'을 누른 뒤: 가입 시도 중엔 허브 Wi-Fi 가 최대 3분 멈춰 이 화면이 잠시 끊깁니다.</p>"
                   "<button class=lnk>Zigbee 가입 시도</button></form>", cap);
    }
    strlcat(o, "</div>", cap);
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
    if (m == s_mask) {
        snprintf(msg, len, "바뀐 것이 없습니다");
        return true;
    }
    esp_err_t e = hub_uplink_set_mask(m);
    if (e != ESP_OK) {
        snprintf(msg, len, "연결 방식을 저장하지 못했습니다(%s)", esp_err_to_name(e));
        return false;
    }
    /* Zigbee 스택·LAN 대기는 부팅 때 고른 통로만 켜므로, 끄고 켜는 것은 재시작으로 깔끔하게 적용한다 */
    xTaskCreate(restart_task, "uplink_rst", 2048, NULL, 5, NULL);
    snprintf(msg, len, "연결 방식을 저장했습니다 (Zigbee %s · LAN %s) — 허브가 재시작해 적용합니다(수십 초)",
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

void hub_uplink_on_request(uint8_t from, const char *text, int len)
{
    if (!(s_mask & from)) {                          /* 끈 통로로 들어온 명령은 따르지 않는다 */
        ESP_LOGW(TAG, "꺼진 통로(%u)의 요청 버림", from);
        return;
    }
    ESP_LOGI(TAG, "요청: %.*s", len, text);
    if (len == 7 && !memcmp(text, "refresh", 7)) {
        hub_sensors_refresh();
    }
    hub_rinnai_command(text, len);
}
