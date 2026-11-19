/*
 * hub_led.c -- 보드의 RGB LED(WS2812, ESP32-C6-DevKitC-1 는 GPIO8) 상태 표시.
 * 온보딩 단계마다 색으로 알려준다: 파랑=검색, 노랑=전송 중, 초록=성공, 빨강=실패.
 * 알람(빨강 깜빡임)은 모든 표시보다 우선하고 재부팅 전까지 유지된다(저장 데이터 오류 → USB 로 점검).
 */
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

#include "hub_led.h"

#define LED_GPIO 8

static led_strip_handle_t s_strip;
static volatile bool s_alarm;
static esp_timer_handle_t s_blink;

static void put(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void blink_cb(void *arg)
{
    static bool lit;
    lit = !lit;
    if (s_strip) {
        put(lit ? 25 : 0, 0, 0);
    }
}

static void alarm_start(void)
{
    if (!s_strip || s_blink) {
        return;
    }
    const esp_timer_create_args_t a = { .callback = blink_cb, .name = "led_alarm" };
    if (esp_timer_create(&a, &s_blink) == ESP_OK) {
        esp_timer_start_periodic(s_blink, 500 * 1000);
    }
}

void hub_led_alarm(void)
{
    if (s_alarm) {
        return;                     /* 한 번 켜지면 재부팅 전까지 유지 */
    }
    s_alarm = true;
    alarm_start();
}

bool hub_led_alarm_on(void) { return s_alarm; }

void hub_led_init(void)
{
    led_strip_config_t sc = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rc = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&sc, &rc, &s_strip) == ESP_OK) {
        led_strip_clear(s_strip);
    }
    if (s_alarm) {
        alarm_start();              /* 초기화 전에 알람이 켜졌으면 지금부터 깜빡임 */
    }
}

void hub_led_set(hub_led_color_t c)
{
    if (!s_strip || s_alarm) {      /* 알람 중엔 다른 표시가 덮지 않게 */
        return;
    }
    static const uint8_t rgb[][3] = {
        [HUB_LED_OFF]    = { 0, 0, 0 },
        [HUB_LED_RED]    = { 10, 0, 0 },
        [HUB_LED_GREEN]  = { 0, 2, 0 },
        [HUB_LED_BLUE]   = { 0, 0, 12 },
        [HUB_LED_YELLOW] = { 9, 4, 0 },
    };
    put(rgb[c][0], rgb[c][1], rgb[c][2]);
}
