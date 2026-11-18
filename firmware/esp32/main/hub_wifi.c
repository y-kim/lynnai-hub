/*
 * hub_wifi.c -- LYNNAI 허브의 Wi-Fi 쪽.
 *
 * 1단계: 설정용 AP(개방)만 띄워 Zigbee 와의 공존과 메모리를 확인한다.
 *  - 채널 1(2.412GHz): SmartThings Station 의 Zigbee 채널 20(2.450GHz)과 멀리 떨어뜨림.
 *  - STA(집 Wi-Fi)는 설정 포털에서 자격증명을 받는 단계에서 추가한다(그때 AP 는 STA 채널을 따름).
 *  - SSID 접미사는 임시로 MAC 끝 3바이트. 제품에선 첫 부팅 때 뽑는 랜덤 기기 ID(NVS)로 바꾼다.
 */
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hub_wifi.h"

static const char *TAG = "HUB_WIFI";

#define HUB_AP_CHANNEL   1
#define HUB_AP_MAX_CONN  4

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        ESP_LOGI(TAG, "AP 접속: " MACSTR " (aid %d)", MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGI(TAG, "AP 해제: " MACSTR " (aid %d)", MAC2STR(e->mac), e->aid);
    } else if (id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP 시작");
    }
}

esp_err_t hub_wifi_start_ap(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   /* 이미 있으면 그대로 사용 */
        return err;
    }
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                        TAG, "event handler");

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), TAG, "read mac");

    wifi_config_t ap = { 0 };
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "LYNNAI-SETUP-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)n;
    ap.ap.channel = HUB_AP_CHANNEL;
    ap.ap.max_connection = HUB_AP_MAX_CONN;
    ap.ap.authmode = WIFI_AUTH_OPEN;           /* 설정용 AP 는 개방(설정 후 전용 WPA2 AP 로 전환) */

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "set config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    ESP_LOGI(TAG, "설정용 AP '%s' 채널 %d (개방)", (char *)ap.ap.ssid, HUB_AP_CHANNEL);
    return ESP_OK;
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
