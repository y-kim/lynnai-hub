/*
 * hub_onboard.c -- 버튼 길게(3초) 누름 → 보일러(GainSpan)를 허브 AP 로 자동 온보딩.
 *
 * 절차(실측): 보일러를 Wi-Fi 리셋 버튼으로 SoftAP(개방, 게이트웨이 192.168.4.1)에 진입시킨 뒤
 *   1) 허브가 'Rinnai…' AP 를 스캔해 찾고
 *   2) STA 를 거기에 잠깐 붙여(개방) 192.168.4.x IP 를 받고
 *   3) POST /gainspan/system/config/network 로 허브 AP(SSID/암호/보안) 를 넘기고
 *   4) 집 Wi-Fi 로 복귀한다.
 * 보일러는 POST 를 받자마자 '검증 없이' SoftAP 를 끄고 그 네트워크로 붙으려 한다. 값이 틀리면
 * 네트워크 없이 멈추므로(버튼 재진입 필요) 보안 타입을 정확히 넣어야 한다.
 * 응답은 비표준이라 파싱하지 않는다 — 전송 후 연결이 끊기면(=SoftAP 내려감) 적용된 것으로 본다.
 */
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "hub_onboard.h"
#include "hub_wifi.h"
#include "hub_zigbee.h"
#include "hub_led.h"
#include "hub_events.h"

static const char *TAG = "HUB_ONBOARD";

#define BOILER_AP_PREFIX   "Rinnai"        /* 보일러 SoftAP SSID 접두사 (확인 필요: 전체 형태) */
#define BOILER_AP_GW       "192.168.4.1"
#define BOILER_SECURITY    "WPA-PERSONAL"   /* 문서 확인: WPA/WPA2 공통. 오픈=NONE, WEP=WEP */

static const hub_state_t *s_st;
static volatile bool s_running;

/* 보일러에 허브 AP 정보를 넘긴다. 성공 = 응답 본문에 <status>ok</status>. */
static bool post_network(const char *ap_ssid, const char *ap_pass)
{
    /* 실제 앱 바디엔 <security> 가 없다(보안 분기는 미호출 죽은 코드). 모듈이 스스로 판단.
     * 안 되면 그때 <security>WPA-PERSONAL</security> 추가 시도(문서 권고 순서). */
    char body[200];
    int bl = snprintf(body, sizeof(body),
                      "<ssid>%s</ssid><password>%s</password>", ap_ssid, ap_pass);
    char req[384];
    int rl = snprintf(req, sizeof(req),
                      "POST http://" BOILER_AP_GW "/gainspan/system/config/network HTTP/1.1\r\n"
                      "Host: ckpush.com\r\nContent-Type: text/xml\r\n"
                      "Content-Length: %d\r\n\r\n%s", bl, body);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = 6 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(80) };
    inet_aton(BOILER_AP_GW, &a.sin_addr);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        ESP_LOGW(TAG, "보일러(%s) 연결 실패", BOILER_AP_GW);
        close(s);
        return false;
    }
    if (send(s, req, rl, 0) != rl) {
        ESP_LOGW(TAG, "전송 실패");
        close(s);
        return false;
    }
    ESP_LOGI(TAG, "허브 AP 정보 전송함 (ssid=%s)", ap_ssid);
    /* 응답은 비표준(본문 앞 \r\n\n). 끊기거나 타임아웃까지 모아서, 본문의 <status>ok</status> 로만 성공 판정 */
    char buf[512];
    int tot = 0;
    while (tot < (int)sizeof(buf) - 1) {
        int n = recv(s, buf + tot, sizeof(buf) - 1 - tot, 0);
        if (n <= 0) {
            break;
        }
        tot += n;
        buf[tot] = 0;
        if (strstr(buf, "</status>")) {
            break;
        }
    }
    close(s);
    buf[tot] = 0;
    for (int i = 0; i < tot; i++) {          /* 한 줄 로그로 보이게 */
        if (buf[i] == '\r' || buf[i] == '\n') {
            buf[i] = ' ';
        }
    }
    ESP_LOGI(TAG, "응답(%d): %s", tot, buf);
    bool ok = strstr(buf, "<status>ok</status>") != NULL;
    if (ok) {
        ESP_LOGI(TAG, "등록 성공(status ok) — 보일러가 SoftAP 끄고 허브 AP 로 전환");
    } else {
        ESP_LOGW(TAG, "status ok 없음 — 등록 안 됨");
    }
    return ok;
}

static void onboard_task(void *arg)
{
    char ssid[33];
    hub_led_set(HUB_LED_BLUE);
    hub_zigbee_ap_onboarding(true);      /* 등록하는 동안 Zigbee 가입 중단 — 라디오를 Wi-Fi 에 양보 */
    ESP_LOGI(TAG, "보일러 온보딩 시작: '%s…' AP 검색", BOILER_AP_PREFIX);

    esp_err_t r = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < 7 && r != ESP_OK; i++) {     /* ~20초간 반복: 버튼을 먼저 눌러도 되게 */
        r = hub_wifi_scan_prefix(BOILER_AP_PREFIX, ssid, sizeof(ssid));
        if (r != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "'%s…' AP 를 못 찾음 — 보일러를 Wi-Fi 리셋(SoftAP)해 두셨나요?", BOILER_AP_PREFIX);
        hub_led_set(HUB_LED_RED); vTaskDelay(pdMS_TO_TICKS(4000)); hub_led_set(HUB_LED_OFF);
        goto done;
    }
    ESP_LOGI(TAG, "보일러 AP 발견: '%s' — 접속 시도", ssid);

    uint32_t ip = 0;
    if (hub_wifi_sta_temp_open(ssid, &ip) != ESP_OK) {
        ESP_LOGW(TAG, "보일러 AP 접속/IP 실패");
        hub_led_set(HUB_LED_RED); vTaskDelay(pdMS_TO_TICKS(4000)); hub_led_set(HUB_LED_OFF);
        goto restore;
    }
    const uint8_t *o = (const uint8_t *)&ip;
    ESP_LOGI(TAG, "보일러 AP 접속됨 (IP %u.%u.%u.%u)", o[0], o[1], o[2], o[3]);

    vTaskDelay(pdMS_TO_TICKS(2000));         /* 앱처럼 접속이 안정된 뒤 전송 */
    hub_led_set(HUB_LED_YELLOW);
    bool ok = post_network(s_st->ap_ssid, s_st->ap_pass);
    hub_led_set(ok ? HUB_LED_GREEN : HUB_LED_RED);
    hub_event(EV_ONBOARD, NULL, ok, 0);

restore:
    ESP_LOGI(TAG, "집 Wi-Fi 로 복귀");
    hub_wifi_apply(s_st);
    vTaskDelay(pdMS_TO_TICKS(5000)); hub_led_set(HUB_LED_OFF);
done:
    hub_zigbee_ap_onboarding(false);     /* 등록 끝 — Zigbee 가입 재개 가능 */
    s_running = false;
    vTaskDelete(NULL);
}

void hub_onboard_init(const hub_state_t *st)
{
    s_st = st;
}

void hub_onboard_trigger(void)
{
    if (!s_st || !s_st->provisioned) {
        ESP_LOGW(TAG, "허브가 아직 설정 전이라 보일러 온보딩 불가");
        return;
    }
    if (s_running) {
        return;
    }
    s_running = true;
    if (xTaskCreate(onboard_task, "hub_onboard", 6144, NULL, 5, NULL) != pdPASS) {
        s_running = false;
    }
}
