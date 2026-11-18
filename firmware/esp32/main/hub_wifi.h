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

/* 허브 AP 쪽 IP(네트워크 바이트 순서) */
uint32_t hub_wifi_ap_ip(void);

/* 10초마다 남은 힙/최저치/가장 큰 블록을 로그로(메모리 예산 확인용) */
void hub_heap_monitor_start(void);
