#pragma once

typedef enum {
    HUB_LED_OFF = 0,
    HUB_LED_RED,
    HUB_LED_GREEN,
    HUB_LED_BLUE,
    HUB_LED_YELLOW,
} hub_led_color_t;

void hub_led_init(void);
void hub_led_set(hub_led_color_t c);
