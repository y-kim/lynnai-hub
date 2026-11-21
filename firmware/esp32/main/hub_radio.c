/*
 * hub_radio.c -- Zigbee 라디오 고르기(내장 / UART 외부). 설명은 hub_radio.h.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hub_radio.h"

static const char *TAG = "HUB_RADIO";

#define IMG_MAGIC   0x524E594Cu        /* "LYNR" */
#define IMG_NATIVE  0
#define IMG_RCP     1
#if CONFIG_ZB_RADIO_SPINEL_UART
#define THIS_IMG    IMG_RCP
#else
#define THIS_IMG    IMG_NATIVE
#endif

/* 이 펌웨어가 어느 라디오용인지: 이미지의 고정된 자리(앱 설명 바로 뒤)에 넣어, 다른 칸의 펌웨어도 읽어 볼 수 있게 한다 */
typedef struct {
    uint32_t magic;
    uint8_t radio;
    uint8_t rsv[3];
} hub_img_desc_t;
const __attribute__((section(".rodata_custom_desc"), used)) hub_img_desc_t hub_img_desc = { .magic = IMG_MAGIC, .radio = THIS_IMG };
#define IMG_DESC_OFF (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

/* 전원을 켠 뒤 펌웨어를 바꾼 횟수(재시작해도 남는 메모리). 탐지가 오락가락해도 끝없이 재시작하지 않게 2번까지만 */
#define SW_MAGIC 0x52535731u
static RTC_NOINIT_ATTR uint32_t s_sw_magic, s_sw_count;

static bool s_present, s_usable, s_probed;
static uint8_t s_force;             /* HUB_RADIO_AUTO / _NATIVE / _EXT (nvs "radio"/"force") */
static int s_other = -1;            /* 다른 칸의 펌웨어 종류(-1 = 없음/모름) */

/* ---------------------------------------------------------------- Spinel(HDLC-lite) 한 번 물어보기 */

static uint16_t fcs16(const uint8_t *d, int n)          /* CRC-16/X-25 */
{
    uint16_t c = 0xFFFF;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
        }
    }
    return (uint16_t)~c;
}

static int hdlc_put(uint8_t *o, int n, uint8_t b)
{
    if (b == 0x7E || b == 0x7D || b == 0x11 || b == 0x13 || b == 0xF8) {
        o[n++] = 0x7D;
        b ^= 0x20;
    }
    o[n++] = b;
    return n;
}

/* 받은 바이트들에서 온전한 Spinel 프레임(FCS 맞고, 머리 바이트의 윗 두 비트가 10)을 하나라도 찾으면 true.
 * reply 면 내 질의에 대한 답(머리 0x81, PROP_VALUE_IS, PROTOCOL_VERSION)만 센다 — 라디오가 리셋될 때 스스로 내는
 * 시작 알림(머리 0x80)은 '라디오→허브' 만 된다는 뜻이라 답으로 치지 않는다(허브→라디오 선이 빠져도 오므로) */
static bool hdlc_has_frame(const uint8_t *rx, int n, bool reply)
{
    uint8_t f[64];
    int fl = -1;
    bool esc = false;
    for (int i = 0; i < n; i++) {
        uint8_t b = rx[i];
        if (b == 0x7E) {
            if (fl >= 4 && (f[0] & 0xC0) == 0x80) {
                uint16_t want = fcs16(f, fl - 2);
                bool mine = fl >= 5 && f[0] == 0x81 && f[1] == 0x06 && f[2] == 0x01;
                if (f[fl - 2] == (want & 0xFF) && f[fl - 1] == (want >> 8) && (mine || !reply)) {
                    return true;
                }
            }
            fl = 0;
            esc = false;
        } else if (fl >= 0) {
            if (b == 0x7D) {
                esc = true;
                continue;
            }
            if (fl < (int)sizeof(f)) {
                f[fl++] = esc ? (uint8_t)(b ^ 0x20) : b;
            } else {
                fl = -1;                                 /* 너무 긴 것은 버리고 다음 구분 바이트부터 */
            }
            esc = false;
        }
    }
    return false;
}

static bool probe(void)
{
    const uart_config_t uc = {
        .baud_rate = HUB_RCP_UART_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(HUB_RCP_UART_PORT, 512, 0, 0, NULL, 0) != ESP_OK) {
        return false;
    }
    bool found = false;
    if (uart_param_config(HUB_RCP_UART_PORT, &uc) == ESP_OK &&
        uart_set_pin(HUB_RCP_UART_PORT, HUB_RCP_UART_TX, HUB_RCP_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK) {
        gpio_set_pull_mode(HUB_RCP_UART_RX, GPIO_PULLUP_ONLY);     /* 아무것도 안 붙었을 때 떠다니지 않게 */
        /* PROP_VALUE_GET(PROTOCOL_VERSION): 머리 0x81(TID 1), 명령 0x02, 속성 0x01 */
        const uint8_t body[3] = { 0x81, 0x02, 0x01 };
        uint16_t fcs = fcs16(body, 3);
        uint8_t tx[16], rx[160];
        int tn = 0;
        tx[tn++] = 0x7E;
        for (int i = 0; i < 3; i++) {
            tn = hdlc_put(tx, tn, body[i]);
        }
        tn = hdlc_put(tx, tn, fcs & 0xFF);
        tn = hdlc_put(tx, tn, fcs >> 8);
        tx[tn++] = 0x7E;
        for (int t = 0; t < 3 && !found; t++) {                    /* RCP 가 같이 켜져 아직 부팅 중일 수 있다 */
            uart_flush_input(HUB_RCP_UART_PORT);
            uart_write_bytes(HUB_RCP_UART_PORT, tx, tn);
            int rn = 0;
            for (int w = 0; w < 5 && !found; w++) {                /* 50ms × 5 */
                int r = uart_read_bytes(HUB_RCP_UART_PORT, rx + rn, sizeof(rx) - rn, pdMS_TO_TICKS(50));
                if (r > 0) {
                    rn += r;
                    found = hdlc_has_frame(rx, rn, true);
                    if (rn >= (int)sizeof(rx)) {
                        rn = 0;
                    }
                }
            }
        }
    }
    uart_driver_delete(HUB_RCP_UART_PORT);          /* 외부용 펌웨어에서는 Zigbee 라이브러리가 다시 연다 */
    if (!found) {
        gpio_reset_pin(HUB_RCP_UART_TX);            /* 없으면 핀을 놓는다 */
        gpio_reset_pin(HUB_RCP_UART_RX);
    }
    return found;
}

/* ---------------------------------------------------------------- 다른 칸의 펌웨어 */

static int image_kind(const esp_partition_t *p)
{
    hub_img_desc_t d;
    if (!p || esp_partition_read(p, IMG_DESC_OFF, &d, sizeof(d)) != ESP_OK || d.magic != IMG_MAGIC || d.radio > IMG_RCP) {
        return -1;
    }
    return d.radio;
}

/* OTA 용: 이 펌웨어의 라디오 변종(0=내장, 1=외부)과, 어떤 칸에 담긴 펌웨어의 변종(-1=없음/깨짐). */
int hub_radio_this_variant(void) { return THIS_IMG; }
int hub_radio_partition_variant(const esp_partition_t *p) { return image_kind(p); }

static const char *force_name(uint8_t f)
{
    return f == HUB_RADIO_NATIVE ? "native" : f == HUB_RADIO_EXT ? "ext" : "auto";
}

void hub_radio_boot(void)
{
    if (s_sw_magic != SW_MAGIC || esp_reset_reason() != ESP_RST_SW) {
        s_sw_magic = SW_MAGIC;
        s_sw_count = 0;
    }
    nvs_handle_t h;
    if (nvs_open("radio", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "force", &s_force) != ESP_OK || s_force > HUB_RADIO_EXT) {
            s_force = HUB_RADIO_AUTO;
        }
        nvs_close(h);
    }
    s_present = probe();
    s_probed = true;
    int want = s_force == HUB_RADIO_NATIVE ? IMG_NATIVE : s_force == HUB_RADIO_EXT ? IMG_RCP : s_present ? IMG_RCP : IMG_NATIVE;
    if (s_force != HUB_RADIO_AUTO) {
        ESP_LOGW(TAG, "라디오 고정 = %s (USB 명령 radio auto 로 풀기)", force_name(s_force));
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (other && run && other->address == run->address) {
        other = NULL;
    }
    s_other = image_kind(other);
    ESP_LOGI(TAG, "외부 라디오 %s · 이 펌웨어 = %s 라디오용(%s) · 다른 칸 = %s", s_present ? "있음" : "없음",
             THIS_IMG == IMG_RCP ? "외부" : "내장", run ? run->label : "?",
             s_other == IMG_RCP ? "외부 라디오용" : s_other == IMG_NATIVE ? "내장 라디오용" : "펌웨어 없음");
    if (want == THIS_IMG) {
        s_usable = THIS_IMG == IMG_NATIVE || s_present;     /* 외부로 고정했는데 라디오가 답하지 않으면 쓸 수 없다 */
        if (!s_usable) {
            ESP_LOGE(TAG, "외부 라디오로 고정돼 있는데 라디오가 답하지 않음 — Zigbee 를 쓰지 않음");
        }
        return;
    }
    if (s_other == want && s_sw_count < 2) {
        esp_err_t e = esp_ota_set_boot_partition(other);       /* 이미지 검사까지 한다 */
        if (e == ESP_OK) {
            s_sw_count++;
            ESP_LOGW(TAG, "%s 라디오용 펌웨어(%s)로 넘어감 — 재시작", want == IMG_RCP ? "외부" : "내장", other->label);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        ESP_LOGE(TAG, "다른 칸으로 넘어가지 못함(%s)", esp_err_to_name(e));
    }
    /* 못 넘어감: 내장용 펌웨어는 내장 라디오로 계속 쓴다. 외부용은 라디오가 답할 때만 쓸 수 있다 */
    s_usable = THIS_IMG == IMG_NATIVE || s_present;
    if (THIS_IMG == IMG_NATIVE) {
        ESP_LOGW(TAG, "외부 라디오용 펌웨어로 넘어가지 못함 — 내장 라디오를 씀");
    } else if (s_usable) {
        ESP_LOGW(TAG, "내장 라디오용 펌웨어로 넘어가지 못함 — 외부 라디오를 씀");
    } else {
        ESP_LOGE(TAG, "외부 라디오가 답하지 않고 내장 라디오용 펌웨어도 없음 — Zigbee 를 쓰지 않음");
    }
}

bool hub_radio_force(uint8_t f)
{
    if (f > HUB_RADIO_EXT) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open("radio", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e = f == HUB_RADIO_AUTO ? nvs_erase_key(h, "force") : nvs_set_u8(h, "force", f);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        e = ESP_OK;
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK) {
        return false;
    }
    s_force = f;
    s_sw_count = 0;                 /* 일부러 바꾼 것이니 '펌웨어 바꾼 횟수' 한도를 다시 센다 */
    return true;
}

/* ---------------------------------------------------------------- 배선 시험(USB 명령 radio test) */

/* pin 을 수신으로만 열어 ms 동안 듣는다: 받은 바이트 수를 돌려주고, 온전한 Spinel 프레임이 있었는지 *frame 에 */
static int listen_pin(int pin, int ms, bool *frame)
{
    static uint8_t rx[256];
    const uart_config_t uc = {
        .baud_rate = HUB_RCP_UART_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    *frame = false;
    if (uart_driver_install(HUB_RCP_UART_PORT, 512, 0, 0, NULL, 0) != ESP_OK) {
        return -1;
    }
    int total = 0, rn = 0;
    if (uart_param_config(HUB_RCP_UART_PORT, &uc) == ESP_OK &&
        uart_set_pin(HUB_RCP_UART_PORT, UART_PIN_NO_CHANGE, pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK) {
        gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
        uart_flush_input(HUB_RCP_UART_PORT);
        for (int w = 0; w < ms / 50; w++) {
            int r = uart_read_bytes(HUB_RCP_UART_PORT, rx + rn, sizeof(rx) - rn, pdMS_TO_TICKS(50));
            if (r > 0) {
                total += r;
                rn += r;
                *frame = *frame || hdlc_has_frame(rx, rn, false);
                if (rn >= (int)sizeof(rx)) {
                    rn = 0;
                }
            }
        }
    }
    uart_driver_delete(HUB_RCP_UART_PORT);
    gpio_reset_pin(pin);
    return total;
}

/* 풀다운을 걸고 읽은 값: 1 이면 상대가 높게 밀고 있다(= 상대의 TX 에 닿아 있다) */
static int level_pd(int pin)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
    vTaskDelay(pdMS_TO_TICKS(30));
    int v = gpio_get_level(pin);
    gpio_reset_pin(pin);
    return v;
}

void hub_radio_wiretest(void)
{
    if (THIS_IMG == IMG_RCP && s_usable) {
        printf("#radio test: 외부 라디오를 쓰는 중이라 시험하지 않음(통신이 되고 있다는 뜻)\n");
        return;
    }
    /* 허브는 듣기만 한다(핀을 밀지 않음). 시험하는 동안 라디오 보드를 리셋하면 시작 신호(Spinel 프레임)를 낸다 */
    int rx_pd = level_pd(HUB_RCP_UART_RX), tx_pd = level_pd(HUB_RCP_UART_TX);
    printf("#radio test: 쉬는 상태(풀다운) rx핀%d=%d tx핀%d=%d — 1 이면 상대 TX 가 닿아 있음\n", HUB_RCP_UART_RX, rx_pd,
           HUB_RCP_UART_TX, tx_pd);
    fflush(stdout);
    bool f_rx, f_tx;
    printf("#radio test: listen rx핀%d\n", HUB_RCP_UART_RX);
    fflush(stdout);
    int n_rx = listen_pin(HUB_RCP_UART_RX, 4000, &f_rx);
    printf("#radio test: listen tx핀%d\n", HUB_RCP_UART_TX);
    fflush(stdout);
    int n_tx = listen_pin(HUB_RCP_UART_TX, 4000, &f_tx);
    bool ans = probe();
    printf("#radio test: rx핀%d bytes=%d frame=%d | tx핀%d bytes=%d frame=%d | 질의 응답=%d\n", HUB_RCP_UART_RX, n_rx, f_rx,
           HUB_RCP_UART_TX, n_tx, f_tx, ans);
    printf("#radio test: %s\n",
           ans ? "정상 — 양방향 통신 됨"
           : f_rx ? "라디오→허브 는 됨. 허브→라디오(허브 TX핀 → 라디오 RX핀) 선을 확인"
           : f_tx ? "선이 엇갈리지 않았음 — 라디오의 TX 가 허브 TX핀에 꽂혀 있음(두 선을 서로 바꿔 꽂기)"
           : "라디오 신호가 어느 핀에도 안 들어옴 — 선·GND·라디오 전원 확인");
    fflush(stdout);
}

bool hub_radio_usable(void) { return s_usable; }
bool hub_radio_external(void) { return THIS_IMG == IMG_RCP; }

void hub_radio_print(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    printf("radio fw=%s slot=%s ext_present=%d usable=%d other=%s switches=%lu force=%s\n", THIS_IMG == IMG_RCP ? "external" : "native",
           run ? run->label : "?", s_probed ? s_present : -1, s_usable,
           s_other == IMG_RCP ? "external" : s_other == IMG_NATIVE ? "native" : "none", (unsigned long)s_sw_count,
           force_name(s_force));
}
