/*
 * hub_led.c -- 보드의 RGB LED(WS2812, ESP32-C6-DevKitC-1 는 GPIO8) 상태 표시.
 * 온보딩 단계마다 색으로 알려준다: 파랑=검색, 노랑=전송 중, 초록=성공, 빨강=실패.
 */
#include "esp_log.h"
#include "led_strip.h"

#include "hub_led.h"

#define LED_GPIO 8

static led_strip_handle_t s_strip;

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
}

void hub_led_set(hub_led_color_t c)
{
    if (!s_strip) {
        return;
    }
    static const uint8_t rgb[][3] = {
        [HUB_LED_OFF]    = { 0, 0, 0 },
        [HUB_LED_RED]    = { 10, 0, 0 },
        [HUB_LED_GREEN]  = { 0, 2, 0 },
        [HUB_LED_BLUE]   = { 0, 0, 12 },
        [HUB_LED_YELLOW] = { 9, 4, 0 },
    };
    led_strip_set_pixel(s_strip, 0, rgb[c][0], rgb[c][1], rgb[c][2]);
    led_strip_refresh(s_strip);
}
