#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "hub_state.h"

/* Wi-Fi 스택·인터페이스(AP+STA) 준비. 한 번만. */
esp_err_t hub_wifi_init(void);

/* 상태대로 시작: 미설정 = 개방 설정 AP / 설정 후 = 집 Wi-Fi(STA) + WPA2 허브 AP */
esp_err_t hub_wifi_start(const hub_state_t *st);

/* 설정이 바뀐 뒤 다시 적용(멈췄다가 새 설정으로 시작) */
esp_err_t hub_wifi_apply(const hub_state_t *st);

#include <stddef.h>

/* prefix 로 시작하는 AP 를 스캔해 찾는다(대소문자 무시). */
esp_err_t hub_wifi_scan_prefix(const char *prefix, char *out, size_t len);

/* STA 를 개방 AP 로 임시 전환하고 IP 를 받는다. 복귀는 hub_wifi_apply(state). */
esp_err_t hub_wifi_sta_temp_open(const char *ssid, uint32_t *ip_out);

/* 허브 AP 쪽 IP(네트워크 바이트 순서) */
uint32_t hub_wifi_ap_ip(void);

/* 10초마다 남은 힙/최저치/가장 큰 블록을 로그로(메모리 예산 확인용) */
void hub_heap_monitor_start(void);
