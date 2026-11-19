#pragma once
#include <stdbool.h>

typedef enum {
    HUB_LED_OFF = 0,
    HUB_LED_RED,
    HUB_LED_GREEN,
    HUB_LED_BLUE,
    HUB_LED_YELLOW,
} hub_led_color_t;

void hub_led_init(void);
void hub_led_set(hub_led_color_t c);

/* 알람: 빨강 깜빡임(다른 표시보다 우선, 재부팅 전까지 유지). 저장 데이터 오류 등 — "USB 연결해 점검" 신호.
 * LED 초기화 전에 불러도 된다(초기화되면 그때부터 깜빡임). */
void hub_led_alarm(void);
bool hub_led_alarm_on(void);
