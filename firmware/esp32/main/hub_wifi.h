#pragma once
#include <stdbool.h>
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
bool hub_wifi_is_ap_peer(uint32_t ip);
bool hub_wifi_ap_peer_is(uint32_t ip, const uint8_t mac[6]);   /* 이 MAC 이 AP 에 붙어 있고 그 주소가 ip 인가 */   /* 상대 IPv4(네트워크 순서)가 허브 AP 대역인가 */

/* 2.4GHz 라디오 공유: Zigbee 가입(steering) 동안 Wi-Fi 를 멈췄다가 다시 켠다. */
void hub_wifi_radio_pause(void);
void hub_wifi_radio_resume(void);
bool hub_wifi_radio_paused(void);     /* 지금 멈춰 둔 상태인가(진단: 이때의 STA 끊김은 오류 아님) */

/* 10초마다 남은 힙/최저치/가장 큰 블록을 로그로(메모리 예산 확인용) */
void hub_heap_monitor_start(void);
