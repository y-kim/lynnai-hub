/*
 * hub_wifi.c -- LYNNAI 허브의 Wi-Fi. 설정 상태(hub_state)에 따라 두 모드.
 *
 *  - 미설정: 개방 설정 AP 'LYNNAI-SETUP-<id>', 채널 1(2.412GHz — SmartThings Zigbee 채널과 떨어뜨림).
 *  - 설정 후: 집 Wi-Fi 에 STA 로 붙고, 허브 전용 WPA2 AP 를 함께 띄운다(APSTA).
 *            AP 채널은 STA 채널을 자동으로 따른다(라디오 하나라 같은 채널이어야 함).
 *  STA 가 끊기면 10초 뒤 재시도: 짧게 반복하면 채널 스캔이 잦아져 AP 와 Zigbee 가 흔들린다.
 */
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hub_wifi.h"

static const char *TAG = "HUB_WIFI";

#define HUB_AP_CHANNEL       1
#define HUB_AP_MAX_CONN      4
#define STA_RETRY_US         (10 * 1000 * 1000)

static esp_netif_t *s_ap_netif;
static bool s_sta_on;
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg)
{
    if (s_sta_on) {
        esp_wifi_connect();
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *e = data;
            ESP_LOGI(TAG, "AP 접속: " MACSTR " (aid %d)", MAC2STR(e->mac), e->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *e = data;
            ESP_LOGI(TAG, "AP 해제: " MACSTR " (aid %d)", MAC2STR(e->mac), e->aid);
            break;
        }
        case WIFI_EVENT_STA_START:
            if (s_sta_on) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *e = data;
            ESP_LOGI(TAG, "집 Wi-Fi 연결됨: 채널 %d (허브 AP 도 이 채널로)", e->channel);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e = data;
            ESP_LOGW(TAG, "집 Wi-Fi 끊김(사유 %d) — 10초 뒤 재시도", e->reason);
            if (s_sta_on) {
                esp_timer_stop(s_retry_timer);
                esp_timer_start_once(s_retry_timer, STA_RETRY_US);
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "집 Wi-Fi IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

esp_err_t hub_wifi_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    /* 허브 AP 주소는 사설 대역(10/8, 172.16/12, 192.168/16)이 아닌 198.18.0.1 (RFC 2544 시험용 대역).
     * Android 는 인터넷 확인용 이름이 '사설 IP'로 풀리면 로그인 망이 아니라 '인터넷 없음'으로 판정하고
     * HTTP 확인 요청을 아예 보내지 않는다 → 설정 페이지가 안 뜬다(실측: DNS 만 오고 요청이 안 옴). */
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_set_ip4_addr(&ip.ip, 198, 18, 0, 1);
    esp_netif_set_ip4_addr(&ip.gw, 198, 18, 0, 1);
    esp_netif_set_ip4_addr(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(s_ap_netif);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip), TAG, "ap ip");
    esp_netif_dhcps_start(s_ap_netif);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.ampdu_rx_enable = 0;     /* 일부 Android 에서 AP 접속 후 데이터가 막히는 문제 회피(널리 쓰는 우회) */
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");  /* 설정은 hub_state 가 관리 */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL), TAG, "wifi ev");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL), TAG, "ip ev");
    const esp_timer_create_args_t t = { .callback = retry_cb, .name = "sta_retry" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&t, &s_retry_timer), TAG, "timer");
    return ESP_OK;
}

esp_err_t hub_wifi_start(const hub_state_t *st)
{
    wifi_config_t ap = { 0 };
    ap.ap.channel = HUB_AP_CHANNEL;
    ap.ap.max_connection = HUB_AP_MAX_CONN;

    if (!st->provisioned) {
        s_sta_on = false;
        hub_state_setup_ssid(st, (char *)ap.ap.ssid, sizeof(ap.ap.ssid));
        ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
        ap.ap.authmode = WIFI_AUTH_OPEN;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap cfg");
        ESP_LOGI(TAG, "설정용 AP '%s' 채널 %d (개방)", (char *)ap.ap.ssid, HUB_AP_CHANNEL);
    } else {
        s_sta_on = true;
        strlcpy((char *)ap.ap.ssid, st->ap_ssid, sizeof(ap.ap.ssid));
        ap.ap.ssid_len = strlen(st->ap_ssid);
        strlcpy((char *)ap.ap.password, st->ap_pass, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

        wifi_config_t sta = { 0 };
        strlcpy((char *)sta.sta.ssid, st->sta_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, st->sta_pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = st->sta_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta.sta.pmf_cfg.capable = true;

        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap cfg");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "sta cfg");
        ESP_LOGI(TAG, "전용 AP '%s'(WPA2) + 집 Wi-Fi '%s' 연결 시도", st->ap_ssid, st->sta_ssid);
    }
    return esp_wifi_start();
}

esp_err_t hub_wifi_apply(const hub_state_t *st)
{
    s_sta_on = false;
    esp_timer_stop(s_retry_timer);
    esp_wifi_stop();
    return hub_wifi_start(st);
}

uint32_t hub_wifi_ap_ip(void)
{
    esp_netif_ip_info_t ip;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        return ip.ip.addr;
    }
    return 0;
}

static void heap_monitor_task(void *arg)
{
    for (;;) {
        ESP_LOGI(TAG, "메모리: 남은 힙 %lu B, 최저 %lu B, 가장 큰 블록 %lu B",
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)esp_get_minimum_free_heap_size(),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void hub_heap_monitor_start(void)
{
    xTaskCreate(heap_monitor_task, "heap_mon", 2560, NULL, 1, NULL);
}
