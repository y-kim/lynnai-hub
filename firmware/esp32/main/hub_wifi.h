#pragma once
#include "esp_err.h"

/* 설정용 Wi-Fi AP(개방)를 띄운다. Zigbee 와 같은 라디오를 시간 분할로 나눠 쓴다. */
esp_err_t hub_wifi_start_ap(void);

/* 10초마다 남은 힙/최저치/가장 큰 블록을 로그로 남긴다(메모리 예산 확인용). */
void hub_heap_monitor_start(void);
