#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HUB_LED_OFF = 0,
    HUB_LED_RED,
    HUB_LED_GREEN,
    HUB_LED_BLUE,
    HUB_LED_YELLOW,
} hub_led_color_t;

void hub_led_init(void);
/* 진행 표시(온보딩·가입). 파랑/노랑 = 진행 중이라 오류 코드보다 우선, 초록/빨강 = 결과라 5초만 보이고 오류 코드에 양보. */
void hub_led_set(hub_led_color_t c);

/*
 * 오류 코드: 빨강 N번 깜빡(0.25초) → 2초 쉼 → 다음 코드. 여러 개면 돌아가며 보여준다.
 *   1 집 Wi-Fi 연결 안 됨       2 Zigbee 미가입(SmartThings 페어링 필요)
 *   3 린나이 기기 연결 끊김      4 저장 데이터 오류(재부팅 전까지 유지)
 * 자세한 내용은 USB 시리얼에 "diag" 를 보내면 나온다(hub_diag.c).
 */
#define HUB_LED_CODE_WIFI   1
#define HUB_LED_CODE_ZIGBEE 2
#define HUB_LED_CODE_RINNAI 3
#define HUB_LED_CODE_STORE  4
#define HUB_LED_CODE_MAX    4
#define HUB_LED_BIT(code)   (1u << ((code) - 1))

void hub_led_codes(uint32_t mask);       /* 지금 켤 코드들(진단 태스크가 주기적으로 갱신). 코드 4 는 따로 래치 */
uint32_t hub_led_codes_get(void);        /* 실제로 보이는 코드들(래치 포함) */
const char *hub_led_code_text(int code);

/* 저장 데이터 오류: 코드 4 를 재부팅 전까지 켠다. LED 초기화 전에 불러도 된다. */
void hub_led_alarm(void);
bool hub_led_alarm_on(void);
