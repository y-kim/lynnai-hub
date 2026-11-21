/*
 * hub_uplink.c -- 통로 고르기 + 보내기/받기 모으기. 설명은 hub_uplink.h.
 */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "hub_store.h"

#include "hub_uplink.h"
#include "hub_zb_rinnai.h"
#include "hub_zigbee.h"
#include "hub_diag.h"
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

void hub_uplink_alert_html(char *o, size_t cap)
{
    if (!hub_zigbee_lost()) {
        return;
    }
    int tries = 0, max = 0;
    long in_s = -1;
    hub_zigbee_restore_state(&tries, &max, &in_s);
    if (in_s >= 0) {                                  /* 되살려 놓고 재시작을 기다리는 중 — 사용자가 할 일 없음 */
        char t2[320];
        snprintf(t2, sizeof(t2),
                 "<div class=card><p class=warn><b>Zigbee 재가입에 실패해 다시 시도합니다</b> (%d/%d)<br><small>"
                 "가입 정보는 되살려 두었습니다. 약 %ld초 뒤 허브가 스스로 다시 시작해 재가입합니다. "
                 "SmartThings 에서 따로 할 일은 없습니다.</small></p></div>", tries, max, in_s);
        strlcat(o, t2, cap);
        return;
    }
    char when[112] = "";
    int kind = 0;
    long ep = 0;
    if (hub_zigbee_last_loss(&kind, NULL, &ep, NULL) && kind == 1 && ep) {
        time_t tt = (time_t)ep;
        struct tm tm;
        localtime_r(&tt, &tm);
        snprintf(when, sizeof(when), "%d월 %d일 %02d:%02d 부팅 때 재가입에 실패했습니다. ", tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min);
    }
    strlcat(o, "<div class=card><p class=warn><b>Zigbee 가입 정보가 사라졌습니다</b> — SmartThings 에서 허브가 "
               "오프라인으로 보입니다.<br><small>", cap);
    strlcat(o, when, cap);
    if (tries >= max) {
        strlcat(o, "가입 정보를 되살려 여러 번 다시 시도했지만 붙지 못했습니다. ", cap);
    }
    strlcat(o, "SmartThings 앱에서 '기기 추가 → 주변 기기 검색'을 연 다음, 아래 '연결 방식'의 "
               "<b>Zigbee 가입 시도</b>를 누르세요. 같은 기기로 다시 붙습니다.</small></p></div>", cap);
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
        hub_zb_diag_t z;
        hub_diag_zb_get(&z);
        if (z.ready && z.joined) {
            char rssi[24] = "";
            if (z.parent_rssi != 127) {
                snprintf(rssi, sizeof(rssi), " · 부모신호 %ddBm", z.parent_rssi);
            }
            snprintf(t, sizeof(t), "<p class=ok>Zigbee 가입됨 · 채널 %u · PAN 0x%04x%s</p>", z.channel, z.pan, rssi);
            strlcat(o, t, cap);
        } else {
            strlcat(o, "<p class=warn>Zigbee 미가입 — SmartThings 에서 오프라인으로 보입니다.</p>", cap);
            uint8_t fs = 0, lt = 0xff;
            uint32_t fc = 0;
            long fage = -1, lage = -1;
            hub_zigbee_last_fail(&fs, &fc, &fage, &lt, &lage);
            if (fc) {
                snprintf(t, sizeof(t), "<p class=muted>마지막 가입 실패: <b>0x%02x</b> — %s<br>(%ld초 전 · 누적 %u회)</p>",
                         fs, hub_zigbee_fail_text(fs), fage, (unsigned)fc);
                strlcat(o, t, cap);
            }
            if (hub_zigbee_auto_stopped()) {
                strlcat(o, "<p class=muted>자동 가입 시도는 멈췄습니다(시도할 때마다 Wi-Fi 가 끊기기 때문). "
                           "SmartThings 앱에서 '기기 추가'를 연 뒤 아래 버튼을 누르세요.</p>", cap);
            }
        }
        {
            int kind = 0;
            unsigned cnt = 0;
            long ep = 0, up = 0;
            if (hub_zigbee_last_loss(&kind, &cnt, &ep, &up)) {     /* 플래시에 남은 기록 — 재부팅해도 보인다 */
                char when[40];
                if (ep) {
                    time_t tt = (time_t)ep;
                    struct tm tm;
                    localtime_r(&tt, &tm);
                    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
                } else {
                    snprintf(when, sizeof(when), "시각 모름(부팅 %ld초째)", up);
                }
                snprintf(t, sizeof(t), "<p class=muted>마지막 가입 정보 삭제: %s — %s (누적 %u회)</p>", when,
                         kind == 1 ? "부팅 재가입 실패로 분실" : "가동 중 탈퇴(SmartThings 에서 삭제 추정)", cnt);
                strlcat(o, t, cap);
            }
        }
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
