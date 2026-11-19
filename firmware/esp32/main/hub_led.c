/*
 * hub_led.c -- 보드의 RGB LED(WS2812, ESP32-C6-DevKitC-1 는 GPIO8) 상태 표시.
 *
 * 두 가지를 보여준다.
 *   진행 표시: 온보딩·가입 단계마다 색(파랑=검색, 노랑=전송 중, 초록=성공, 빨강=실패)  — hub_led_set
 *   오류 코드: 빨강 N번 깜빡 → 2초 쉼 (여러 개면 돌아가며)                           — hub_led_codes
 * 진행 중(파랑/노랑)이면 진행 표시가 이기고, 결과(초록/빨강)는 5초 뒤 오류 코드에 자리를 내준다.
 * 오류가 없으면 마지막 진행 표시 색이 그대로 남는다.
 */
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "led_strip.h"

#include "hub_led.h"

#define LED_GPIO 8
#define TICK_MS  250
#define GAP_TICKS 8                 /* 코드 사이 쉼 2초 */
#define RESULT_HOLD_US (5 * 1000 * 1000)

static led_strip_handle_t s_strip;
static SemaphoreHandle_t s_mx;
static esp_timer_handle_t s_tick;

static volatile uint32_t s_mask;    /* 진단 태스크가 정한 코드들 */
static volatile bool s_store;       /* 코드 4 래치 */
static volatile hub_led_color_t s_base;
static volatile int64_t s_hold_until;
static bool s_showing;              /* 지금 오류 코드를 그리는 중 */
static int s_code, s_step;

static const uint8_t RGB[][3] = {
    [HUB_LED_OFF]    = { 0, 0, 0 },
    [HUB_LED_RED]    = { 10, 0, 0 },
    [HUB_LED_GREEN]  = { 0, 2, 0 },
    [HUB_LED_BLUE]   = { 0, 0, 12 },
    [HUB_LED_YELLOW] = { 9, 4, 0 },
};

static void put(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

uint32_t hub_led_codes_get(void)
{
    return s_mask | (s_store ? HUB_LED_BIT(HUB_LED_CODE_STORE) : 0);
}

static int next_code(uint32_t m, int after)
{
    for (int i = 1; i <= HUB_LED_CODE_MAX; i++) {
        int c = (after + i - 1) % HUB_LED_CODE_MAX + 1;
        if (m & HUB_LED_BIT(c)) {
            return c;
        }
    }
    return 0;
}

static void tick_cb(void *arg)
{
    if (!s_strip || xSemaphoreTake(s_mx, 0) != pdTRUE) {
        return;                     /* hub_led_set 이 그리는 중이면 이번 틱은 건너뜀 */
    }
    uint32_t m = hub_led_codes_get();
    hub_led_color_t base = s_base;
    bool hold = base == HUB_LED_BLUE || base == HUB_LED_YELLOW ||
                (base != HUB_LED_OFF && esp_timer_get_time() < s_hold_until);
    if (hold || !m) {
        if (s_showing) {            /* 코드 표시를 멈추고 진행 표시 색으로 복귀 */
            s_showing = false;
            put(RGB[base][0], RGB[base][1], RGB[base][2]);
        }
        xSemaphoreGive(s_mx);
        return;
    }
    if (!s_showing) {
        s_showing = true;
        s_step = 0;
    }
    if (s_step == 0) {
        s_code = next_code(m, s_code);  /* 한 코드를 다 보여준 뒤에만 다음으로 */
    }
    int blinks = s_code * 2;        /* 켜기·끄기 한 쌍씩 */
    bool on = s_step < blinks && (s_step % 2) == 0;
    put(on ? 25 : 0, 0, 0);
    if (++s_step >= blinks + GAP_TICKS) {
        s_step = 0;
    }
    xSemaphoreGive(s_mx);
}

static void tick_start(void)
{
    if (!s_strip || s_tick) {
        return;
    }
    const esp_timer_create_args_t a = { .callback = tick_cb, .name = "led_code" };
    if (esp_timer_create(&a, &s_tick) == ESP_OK) {
        esp_timer_start_periodic(s_tick, TICK_MS * 1000);
    }
}

void hub_led_codes(uint32_t mask) { s_mask = mask; }

const char *hub_led_code_text(int code)
{
    switch (code) {
    case HUB_LED_CODE_WIFI:   return "집 Wi-Fi 연결 안 됨";
    case HUB_LED_CODE_ZIGBEE: return "Zigbee 미가입(SmartThings 페어링 필요)";
    case HUB_LED_CODE_RINNAI: return "린나이 기기 연결 끊김";
    case HUB_LED_CODE_STORE:  return "저장 데이터 오류";
    default:                  return "?";
    }
}

void hub_led_alarm(void) { s_store = true; }      /* 재부팅 전까지 유지 */

bool hub_led_alarm_on(void) { return s_store; }

void hub_led_init(void)
{
    s_mx = xSemaphoreCreateMutex();
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
    tick_start();
}

void hub_led_set(hub_led_color_t c)
{
    if (!s_strip) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_base = c;
    s_hold_until = esp_timer_get_time() + RESULT_HOLD_US;
    s_showing = false;              /* 오류 코드는 다음 틱에 처음부터 다시 */
    put(RGB[c][0], RGB[c][1], RGB[c][2]);
    xSemaphoreGive(s_mx);
}
