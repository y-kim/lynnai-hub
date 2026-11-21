/*
 * hub_sensors.c -- 허브에 직접 꽂는 센서: 온습도계(SHT31) + 가스 계량기 펄스(리드 스위치).
 * Pi 의 roomsensor.py / gasmeter.py 를 옮긴 것. 값은 hub_uplink 로 SmartThings 에 보낸다(Zigbee/LAN).
 *
 * 각각 '플러그인'이라 관리 화면(/admin)에서 추가·삭제하고 핀을 바꾼다(hub_plugins.c, 기본값 아래).
 *   SHT31-D  : I2C 0x44, SDA = GPIO6, SCL = GPIO7, 3.3V. 1분마다 잰다.
 *   가스 계량기: 리드 스위치 한쪽 = GPIO10, 다른 쪽 = GND (내부 풀업). 1펄스 = 0.01 m³.
 *              닫힘 0.1초 이상 + 그 전에 열림 0.3초 이상이어야 1개로 센다(접점 떨림·자석 앞 정지 대비).
 *              누적 개수와 보정값(계량기 숫자)은 NVS 에 저장해 재부팅해도 이어진다.
 *
 *   가스 값은 허브 공용 서버 API(/api/v1/state·/metrics)로 나간다(플러그인이 자기 경로를 열지 않는다).
 *   계량기 숫자 맞추기는 관리 화면(로그인 필요)에서.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "hub_store.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "hub_sensors.h"
#include "hub_uplink.h"
#include "hub_plugins.h"
#include "hub_expose.h"
#include "hub_events.h"
#include "hub_api.h"

static const char *TAG = "HUB_SENSORS";

#define SHT_ADDR     0x44

/* 핀·간격·펄스 양은 플러그인 설정(관리 화면, hub_plugins.c)에서 온다 */
static int s_sda, s_scl, s_every_s, s_gas_pin;
static uint32_t s_step_l;
#define GAS_STEP_L   s_step_l
#define GAS_LOW_MS   100
#define GAS_HIGH_MS  300
#define GAS_POLL_MS  20
#define FLOW_STOP_S  900              /* 이만큼 펄스가 없으면 유량 0 */
/* 고장 판정. 온습도계: 읽기 실패하면 20초 뒤 다시, 3번 연속이면 오프라인(그 뒤로는 30초마다 다시 본다).
 * 가스 계량기: '난방 중' 인 시간(펄스가 오면 0부터 다시)이 한도를 넘도록 펄스가 없으면 오프라인.
 *   한도 = 1펄스 양(L) × 180초, 최소 30분 — 난방 중이면 평균 20 L/h 는 쓴다고 보고 넉넉히 잡는다(10 L 펄스 = 30분).
 *   버너가 켜졌다 꺼졌다 하는 것을 감안한 값. 온수만 쓸 때는 보일러 상태에 표시가 없어 감시하지 못한다. */
#define ROOM_FAIL_N       3
#define ROOM_RETRY_S      20
#define ROOM_RETRY_BAD_S  30
#define GAS_FAULT_MIN_S   1800
#define GAS_FAULT_S_PER_L 180
#define GAS_WATCH_S       5
#define RING         1024             /* 최근 24시간 펄스 시각(초). 넘치면 '하루' 가 적게 나온다 */
#define NS           "gas"
#define UNSET        0xFFFFFFFFu

static SemaphoreHandle_t s_lock;
static uint32_t s_count;              /* 누적 펄스 */
static uint32_t s_base_l = UNSET;     /* 보정: 이 개수일 때 계량기 숫자(L) */
static uint32_t s_base_count;
static uint32_t s_ring[RING];
static uint32_t s_ring_n, s_ring_head;
static uint32_t s_last_s, s_gap_s;    /* 마지막 펄스 시각(부팅 후 초), 직전 간격 */
static volatile bool s_gas_dirty, s_plug_dirty;
static uint32_t s_count_unsaved;     /* 아직 NVS 에 안 쓴 펄스 수 */
/* 센서마다 독립: 세대 번호가 바뀌면 그 센서 태스크만 스스로 종료한다(다른 센서는 안 건드림). */
static volatile uint32_t s_gas_gen, s_room_gen;
static volatile bool s_gas_alive, s_room_alive;
/* 지금 돌고 있는 설정(바뀐 센서만 다시 적용하려고 비교용) */
static bool s_run_gas_on, s_run_room_on;
static int s_run_gas_pin = -1, s_run_sda = -1, s_run_scl = -1;
static int16_t s_room_t100, s_room_h100;      /* 마지막 온습도(서버 API 용) */
static int64_t s_room_at_us;
static volatile bool s_room_fault, s_gas_fault;
static volatile int s_room_fails;             /* 연속 읽기 실패 */
static volatile uint32_t s_gas_heat_s;        /* 마지막 펄스 뒤로 '난방 중' 이었던 시간(초) */
static volatile bool s_heating_now;           /* 마지막으로 본 '난방 중'(diag 용) */

uint8_t hub_sensors_faults(void)
{
    return (uint8_t)((s_room_fault && s_run_room_on ? PLUG_ROOM : 0) | (s_gas_fault && s_run_gas_on ? PLUG_GAS : 0));
}

/* 플랫폼에 알림: [켜진 플러그인 비트][고장 비트]. 옛 드라이버는 첫 바이트만 본다 */
static void send_plugins(void)
{
    uint8_t m[2] = { hub_plugins_mask(), hub_sensors_faults() };
    hub_uplink_send(HUB_RK_CMD_PLUGINS, m, 2);
}

static uint32_t gas_fault_limit_s(void)
{
    uint32_t lim = s_step_l * GAS_FAULT_S_PER_L;
    return lim < GAS_FAULT_MIN_S ? GAS_FAULT_MIN_S : lim;
}

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

/* ---- 재부팅을 넘기는 사용량(지난 하루·한 시간) ----
 * 펄스 시각(s_ring)은 RAM 에만 있어서, 재부팅하면 '지난 하루'와 '지난 한 시간'이 0 부터 다시 셌다(재부팅 직후엔 둘이
 * 같은 값). 10분 칸 144개(24시간)에 칸마다 펄스 수를 담아 플래시에 둔다. 칸은 벽시계 기준(epoch/600)이라 시계가 맞은
 * 뒤에만 쓰고 읽는다.
 *   s_carry   부팅 때 플래시에서 읽은 것 = 이번 부팅 전의 펄스(그 뒤로 안 바뀜)
 *   저장      s_carry + 이번 부팅의 펄스(s_ring)를 지금 시각 기준으로 다시 깔아서 쓴다
 * 저장은 10분 칸이 넘어갈 때와 일부러 재시작하기 직전에만(플래시를 아낀다) — 갑자기 꺼지면 마지막 10분쯤은 빠진다. */
#define BK_S     600
#define BK_N     144
#define BK_MAGIC 0x676b3031u          /* "gk01" */
typedef struct {
    uint32_t magic;
    uint32_t last;                    /* b[0] 이 가리키는 칸 번호(epoch / BK_S). b[k] 는 칸 (last - k) */
    uint8_t  b[BK_N];
} gas_bk_t;
static gas_bk_t s_carry;              /* magic 이 맞을 때만 유효 */
static uint32_t s_bk_saved_idx;       /* 마지막으로 저장한 때의 칸 번호 */
static volatile bool s_bk_dirty;      /* 저장한 뒤로 펄스가 있었다 */

static bool wall_now(uint32_t *epoch)
{
    time_t t = time(NULL);
    if (t < 1700000000) {
        return false;                 /* 시계가 아직 안 맞음 */
    }
    *epoch = (uint32_t)t;
    return true;
}

/* 이번 부팅 전의 펄스 가운데 지난 하루·한 시간에 드는 수(칸 단위 — 10분까지 어긋날 수 있다). 호출자가 s_lock 보유 */
static void carry_counts(uint32_t *hour, uint32_t *day)
{
    uint32_t now;
    *hour = *day = 0;
    if (s_carry.magic != BK_MAGIC || !wall_now(&now)) {
        return;
    }
    for (int k = 0; k < BK_N; k++) {
        if (!s_carry.b[k] || s_carry.last < (uint32_t)k) {
            continue;
        }
        uint32_t end = (s_carry.last - (uint32_t)k + 1) * BK_S;      /* 그 칸이 끝나는 시각 */
        if (end > now) {
            end = now;
        }
        if (now - end < 86400) {
            *day += s_carry.b[k];
        }
        if (now - end < 3600) {
            *hour += s_carry.b[k];
        }
    }
}

/* 지금 시각 기준으로 칸을 다시 깔아 플래시에 쓴다(시계가 안 맞으면 건너뜀) */
static void bk_save(void)
{
    uint32_t now;
    if (!wall_now(&now)) {
        return;
    }
    gas_bk_t *n = calloc(1, sizeof(*n));
    if (!n) {
        return;
    }
    n->magic = BK_MAGIC;
    n->last = now / BK_S;
    uint32_t up = now_s();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_carry.magic == BK_MAGIC) {                 /* 이번 부팅 전의 것: 칸 번호만 옮긴다 */
        for (int k = 0; k < BK_N; k++) {
            if (s_carry.b[k] && s_carry.last >= (uint32_t)k) {
                uint32_t idx = s_carry.last - (uint32_t)k;
                if (idx <= n->last && n->last - idx < BK_N) {
                    n->b[n->last - idx] = s_carry.b[k];
                }
            }
        }
    }
    for (uint32_t i = 0; i < s_ring_n; i++) {        /* 이번 부팅의 펄스: 부팅 후 초 → 벽시계 → 칸 */
        uint32_t ts = s_ring[(s_ring_head + RING - 1 - i) % RING];
        uint32_t idx = (now - (up - ts)) / BK_S;
        if (idx <= n->last && n->last - idx < BK_N && n->b[n->last - idx] < 0xff) {
            n->b[n->last - idx]++;
        }
    }
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "bk", n, sizeof(*n));
        nvs_commit(h);
        nvs_close(h);
        s_bk_saved_idx = n->last;
        s_bk_dirty = false;
    }
    free(n);
}

/* ---------------------------------------------------------------- 저장 */

static void gas_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        hs_get_u32(h, NS, "count", &s_count);
        hs_get_u32(h, NS, "base_l", &s_base_l);
        hs_get_u32(h, NS, "base_cnt", &s_base_count);
        size_t sz = sizeof(s_carry);
        if (nvs_get_blob(h, "bk", &s_carry, &sz) != ESP_OK || sz != sizeof(s_carry) || s_carry.magic != BK_MAGIC) {
            memset(&s_carry, 0, sizeof(s_carry));    /* 없거나 다른 형식 — 이번 부팅부터 센다 */
        }
        nvs_close(h);
    }
}

static void gas_save(bool base)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    hs_set_u32(h, "count", s_count);
    if (base) {
        hs_set_u32(h, "base_l", s_base_l);
        hs_set_u32(h, "base_cnt", s_base_count);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------------------------------------------------------- 가스 */

static gas_view_t gas_view(void)
{
    gas_view_t v = { 0 };
    uint32_t t = now_s(), hour = 0, day = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    v.count = s_count;
    v.reading_l = s_base_l == UNSET ? UNSET : s_base_l + (s_count - s_base_count) * GAS_STEP_L;
    for (uint32_t i = 0; i < s_ring_n; i++) {
        uint32_t ts = s_ring[(s_ring_head + RING - 1 - i) % RING];
        if (t - ts <= 3600) {
            hour++;
        }
        if (t - ts <= 86400) {
            day++;
        }
    }
    {
        uint32_t ch, cd;                             /* 이번 부팅 전의 펄스(플래시에서 읽은 것) */
        carry_counts(&ch, &cd);
        hour += ch;
        day += cd;
    }
    if (s_last_s && t - s_last_s <= FLOW_STOP_S) {
        uint32_t since = t - s_last_s;
        uint32_t gap = s_gap_s > since ? s_gap_s : since;
        if (gap < 1) {
            gap = 1;
        }
        v.flow_lph = GAS_STEP_L * 3600 / gap;
    }
    xSemaphoreGive(s_lock);
    v.hour_l = hour * GAS_STEP_L;
    v.day_l = day * GAS_STEP_L;
    return v;
}

static void gas_pulse(void)
{
    uint32_t t = now_s();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count++;
    s_gap_s = s_last_s ? t - s_last_s : 0;
    s_last_s = t;
    s_ring[s_ring_head] = t;
    s_ring_head = (s_ring_head + 1) % RING;
    if (s_ring_n < RING) {
        s_ring_n++;
    }
    xSemaphoreGive(s_lock);
    s_count_unsaved++;        /* NVS 저장은 펄스마다 하지 않고 모아서(아래) — Zigbee 자격증명 있는 플래시 마모·간섭 줄임 */
    s_bk_dirty = true;
    s_gas_heat_s = 0;
    if (s_gas_fault) {        /* 펄스가 다시 온다: 값 보고보다 먼저 '정상' 을 알린다 */
        s_gas_fault = false;
        ESP_LOGI(TAG, "가스 계량기: 펄스가 다시 들어옴 — 정상");
        send_plugins();
    }
    s_gas_dirty = true;
    hub_event(EV_GAS_PULSE, NULL, (int32_t)s_count, (int32_t)s_step_l);   /* 서버가 정확한 시각으로 다시 셀 수 있게 */
    ESP_LOGI(TAG, "가스 펄스 %lu", (unsigned long)s_count);
}

static void send_gas(void);

/* 일부러 재시작하기 직전(OTA·재부팅 버튼·Zigbee 되살리기): 아직 안 쓴 누적 펄스와 사용량 칸을 플래시에 쓴다 */
void hub_sensors_flush(void)
{
    if (!s_lock || !s_run_gas_on) {
        return;
    }
    if (s_count_unsaved) {
        s_count_unsaved = 0;
        gas_save(false);
    }
    bk_save();
}

/* GAS_WATCH_S 초마다: 난방 중인 시간을 쌓고, 한도를 넘도록 펄스가 없으면 고장으로 본다 */
static void gas_watch(void)
{
    s_heating_now = hub_api_heating();
    if (s_gas_fault || !s_heating_now) {
        return;
    }
    s_gas_heat_s += GAS_WATCH_S;
    if (s_gas_heat_s >= gas_fault_limit_s()) {
        s_gas_fault = true;
        ESP_LOGW(TAG, "가스 계량기: 난방 중 %lu분 동안 펄스 없음 — 오프라인으로 알림(스위치 선·자석 위치 확인)",
                 (unsigned long)(s_gas_heat_s / 60));
        send_plugins();
    }
}

static void gas_task(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    int pin = s_gas_pin;
    s_gas_alive = true;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int low_ms = 0, high_ms = 0;
    bool armed = false;
    int64_t next_gas = 0, next_save = 60LL * 1000000, next_watch = 0, next_bk = 0;
    for (;;) {
        if (gen != s_gas_gen) {                        /* 가스 설정이 바뀜 — 이 태스크만 종료 */
            s_gas_alive = false;
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(GAS_POLL_MS));
        if (gpio_get_level(pin) == 0) {                /* 자석이 스위치 앞: 닫힘 */
            high_ms = 0;
            low_ms += GAS_POLL_MS;
            if (armed && low_ms >= GAS_LOW_MS) {
                armed = false;
                gas_pulse();
            }
        } else {
            low_ms = 0;
            high_ms += GAS_POLL_MS;
            if (high_ms >= GAS_HIGH_MS) {
                armed = true;
            }
        }
        /* 보고: 펄스가 오면 곧바로(최소 5초 간격), 아니면 1분마다. 센 적·보정한 적 없으면 안 보냄 */
        int64_t t = esp_timer_get_time();
        if (t >= next_watch) {
            next_watch = t + GAS_WATCH_S * 1000000LL;
            gas_watch();
        }
        if ((s_count || s_base_l != UNSET) && (t >= next_gas || (s_gas_dirty && t >= next_gas - 55LL * 1000000))) {
            s_gas_dirty = false;
            next_gas = t + 60LL * 1000000;
            send_gas();
        }
        if (s_count_unsaved && t >= next_save) {       /* 모아서 저장(최대 60초에 한 번) */
            next_save = t + 60LL * 1000000;
            s_count_unsaved = 0;
            gas_save(false);
        }
        if (s_bk_dirty && t >= next_bk) {              /* 10분 칸이 넘어갔으면 사용량 칸도 저장 */
            uint32_t now;
            next_bk = t + 30LL * 1000000;
            if (wall_now(&now) && now / BK_S != s_bk_saved_idx) {
                bk_save();
            }
        }
    }
}

static void send_gas(void)
{
    gas_view_t v = gas_view();
    uint32_t p[5] = { v.count, v.reading_l, v.hour_l, v.day_l, v.flow_lph };
    if (s_gas_fault) {
        send_plugins();       /* 고장 중엔 값보다 먼저 다시 알린다(드라이버가 재시작해 잊었어도 온라인으로 되돌리지 않게) */
    }
    hub_uplink_send(HUB_RK_CMD_GAS, (uint8_t *)p, sizeof(p));   /* 리틀엔디언 u32 x5 */
}

/* ---------------------------------------------------------------- 온습도 */

static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t c = 0xff;
    for (int i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) {
            c = c & 0x80 ? (c << 1) ^ 0x31 : c << 1;
        }
    }
    return c;
}

static i2c_master_dev_handle_t s_sht;
static i2c_master_bus_handle_t s_bus;

static bool sht_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = -1,
        .sda_io_num = s_sda,
        .scl_io_num = s_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (!s_bus && i2c_new_master_bus(&bc, &s_bus) != ESP_OK) {      /* 실패하면 다음 차례에 다시 해 본다 */
        s_bus = NULL;
        return false;
    }
    if (s_sht) {
        return true;
    }
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = SHT_ADDR, .scl_speed_hz = 100000 };
    if (i2c_master_bus_add_device(s_bus, &dc, &s_sht) != ESP_OK) {
        s_sht = NULL;
        return false;
    }
    return true;
}

static bool sht_read(float *t, float *h)
{
    const uint8_t cmd[2] = { 0x24, 0x00 };              /* 한 번 재기, 높은 정밀도, 클럭 늘이기 없음 */
    uint8_t d[6];
    if (i2c_master_transmit(s_sht, cmd, 2, 100) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    if (i2c_master_receive(s_sht, d, 6, 100) != ESP_OK || crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) {
        return false;
    }
    *t = -45 + 175 * ((d[0] << 8) | d[1]) / 65535.0f;
    *h = 100 * ((d[3] << 8) | d[4]) / 65535.0f;
    return true;
}

/* ---------------------------------------------------------------- 보내기 */

/* 온습도계 전용 태스크: SHT 초기화(자기 I2C 버스)·읽기·보고. 가스와 독립. */
static void room_task(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    s_room_alive = true;
    bool sht_seen = false;
    int64_t next_sht = 0;
    for (;;) {
        if (gen != s_room_gen) {                       /* 온습도 설정이 바뀜 — 이 태스크만 종료 */
            s_room_alive = false;
            vTaskDelete(NULL);
        }
        int64_t t = esp_timer_get_time();
        if (t >= next_sht) {
            float tc, rh;
            if (sht_init() && sht_read(&tc, &rh)) {
                next_sht = t + (int64_t)s_every_s * 1000000;   /* 간격은 매번 다시 읽음(재시작 불필요) */
                if (!sht_seen || s_room_fault) {
                    ESP_LOGI(TAG, "SHT31 %s: %.1f°C %.0f%%", s_room_fault ? "다시 응답" : "찾음", tc, rh);
                }
                sht_seen = true;
                s_room_fails = 0;
                if (s_room_fault) {                    /* 값 보고보다 먼저 '정상' 을 알린다 */
                    s_room_fault = false;
                    send_plugins();
                }
                int16_t p[2] = { (int16_t)(tc * 100), (int16_t)(rh * 100) };
                s_room_t100 = p[0];
                s_room_h100 = p[1];
                s_room_at_us = esp_timer_get_time();
                hub_uplink_send(HUB_RK_CMD_ROOM, (uint8_t *)p, sizeof(p));
            } else {
                int retry = s_room_fault ? ROOM_RETRY_BAD_S : ROOM_RETRY_S;
                if (retry > s_every_s) {
                    retry = s_every_s;
                }
                next_sht = t + (int64_t)retry * 1000000;
                if (s_room_fails < ROOM_FAIL_N && ++s_room_fails == ROOM_FAIL_N) {
                    s_room_fault = true;
                    ESP_LOGW(TAG, "SHT31 이 %d번 연속 응답하지 않음 — 오프라인으로 알림(배선 3V3·GND·SDA %d·SCL %d 확인)",
                             ROOM_FAIL_N, s_sda, s_scl);
                    send_plugins();
                } else if (sht_seen && s_room_fails == 1) {
                    ESP_LOGW(TAG, "SHT31 읽기 실패 — %d초 뒤 다시", retry);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* 허브 수준(센서 아님): 켜진 플러그인 목록 + 노출 설정을 플랫폼에 알린다. 센서 재적용과 무관하게 계속 돈다. */
static void announce_task(void *arg)
{
    int64_t next = 10LL * 1000000;
    for (;;) {
        int64_t t = esp_timer_get_time();
        if (t >= next || s_plug_dirty) {
            s_plug_dirty = false;
            next = t + 300LL * 1000000;
            send_plugins();
            hub_expose_send_all();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool hub_sensors_room(float *t, float *h, int *age_s)
{
    if (!s_room_at_us || !hub_plugin_enabled("room")) {
        return false;
    }
    *t = s_room_t100 / 100.0f;
    *h = s_room_h100 / 100.0f;
    *age_s = (int)((esp_timer_get_time() - s_room_at_us) / 1000000);
    return true;
}

bool hub_sensors_gas(gas_view_t *v, uint32_t *step_l)
{
    if (!s_lock || !hub_plugin_enabled("gas")) {
        return false;
    }
    *v = gas_view();
    *step_l = s_step_l;
    return true;
}

/* 관리 화면 카드의 상태 줄 */
static bool room_status(char *out, size_t cap)
{
    if (s_room_fault) {
        snprintf(out, cap, "센서가 응답하지 않습니다. 배선(3V3·GND·SDA·SCL)을 확인하세요.");
        return false;
    }
    if (!s_room_at_us) {
        snprintf(out, cap, "확인 중…");
        return true;
    }
    snprintf(out, cap, "정상 · %.1f°C · %.0f%% (%d초 전)", s_room_t100 / 100.0f, s_room_h100 / 100.0f,
             (int)((esp_timer_get_time() - s_room_at_us) / 1000000));
    return true;
}

static bool gas_status(char *out, size_t cap)
{
    uint32_t last = s_last_s, t = now_s();
    if (s_gas_fault) {
        snprintf(out, cap, "난방 중 %lu분 동안 펄스가 없습니다. 스위치 선과 자석 위치를 확인하세요.",
                 (unsigned long)(s_gas_heat_s / 60));
        return false;
    }
    if (!last) {
        snprintf(out, cap, "정상 · 누적 %lu펄스 · 켜진 뒤 펄스 없음", (unsigned long)s_count);
    } else {
        snprintf(out, cap, "정상 · 누적 %lu펄스 · 마지막 펄스 %lu분 전", (unsigned long)s_count, (unsigned long)((t - last) / 60));
    }
    return true;
}

void hub_sensors_print(void)
{
    printf("sensor room on=%d fault=%d fails=%d age=%llds\n", s_run_room_on, s_room_fault, s_room_fails,
           s_room_at_us ? (long long)((esp_timer_get_time() - s_room_at_us) / 1000000) : -1LL);
    printf("sensor gas on=%d fault=%d pin=%d level=%d pulses=%lu last=%lds heating=%d heat_s=%lu limit_s=%lu\n", s_run_gas_on,
           s_gas_fault, s_run_gas_pin, s_run_gas_on && s_run_gas_pin >= 0 ? gpio_get_level(s_run_gas_pin) : -1,
           (unsigned long)s_count, s_last_s ? (long)(now_s() - s_last_s) : -1L, s_heating_now,
           (unsigned long)s_gas_heat_s, (unsigned long)gas_fault_limit_s());
}

void hub_sensors_refresh(void)
{
    s_gas_dirty = true;
    s_plug_dirty = true;
}

/* 가스 플러그인의 '즉시 동작'(관리 화면): 지금 계량기 숫자에 맞춘다 */
static bool gas_calibrate(const char *value, char *msg, size_t msglen)
{
    char v[32];
    strlcpy(v, value, sizeof(v));
    for (char *c = v; *c; c++) {
        if (*c == ',') *c = '.';
    }
    char *end;
    double m3 = strtod(v, &end);
    if (end == v || *end || !isfinite(m3) || !hub_sensors_set_reading(m3)) {   /* "nan"·"inf" 도 거절 */
        snprintf(msg, msglen, "계량기 숫자를 확인해 주세요 (예: 1676.47)");
        return false;
    }
    snprintf(msg, msglen, "계량기 숫자를 %.2f ㎥ 로 맞췄습니다", m3);
    return true;
}

/* 관리 화면에서: 지금 계량기 숫자에 맞춘다 */
bool hub_sensors_set_reading(double m3)
{
    if (!isfinite(m3) || m3 <= 0 || m3 > 4000000) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_base_l = (uint32_t)(m3 * 1000 + 0.5);
    s_base_count = s_count;
    xSemaphoreGive(s_lock);
    gas_save(true);
    s_gas_dirty = true;
    ESP_LOGI(TAG, "가스 계량기 보정: %.2f m³ (펄스 %lu)", m3, (unsigned long)s_count);
    return true;
}


/* ---- 가스 센서: 자기 세대만 다룬다(온습도는 안 건드림) ---- */
static void gas_apply(void)
{
    bool want = hub_plugin_enabled("gas");
    int pin = hub_plugin_get("gas", "pin");
    s_step_l = hub_plugin_get("gas", "step");       /* 펄스 양은 라이브로 읽히므로 재시작 불필요 */
    if (want == s_run_gas_on && (!want || pin == s_run_gas_pin)) {
        return;                                     /* 켜짐·핀 그대로면 가스 태스크는 안 건드림 */
    }
    s_gas_gen++;                                    /* 돌던 가스 태스크만 종료 */
    s_gas_fault = false;                            /* 켜짐·핀이 바뀌면 처음부터 다시 본다 */
    s_gas_heat_s = 0;
    for (int i = 0; i < 40 && s_gas_alive; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (s_run_gas_on && s_run_gas_pin >= 0) {
        gpio_reset_pin(s_run_gas_pin);              /* 옛 핀 되돌림 */
    }
    s_gas_pin = pin;
    s_run_gas_on = want;
    s_run_gas_pin = pin;
    if (want) {
        ESP_LOGI(TAG, "가스 계량기: 핀 %d, 1펄스 %lu L, 누적 %lu", pin, (unsigned long)s_step_l, (unsigned long)s_count);
        hub_plugin_register_action("gas", "계량기 숫자 맞추기 (㎥)", "예: 1676.47", gas_calibrate);
        hub_plugin_register_status("gas", gas_status);
        xTaskCreate(gas_task, "hub_gas", 4096, (void *)(uintptr_t)s_gas_gen, 6, NULL);
    } else {
        ESP_LOGI(TAG, "가스 계량기 꺼짐");
    }
}

/* ---- 온습도 센서: 자기 세대만 다룬다(가스는 안 건드림). 간격 변경은 라이브라 재시작 불필요 ---- */
static void room_apply(void)
{
    bool want = hub_plugin_enabled("room");
    int sda = hub_plugin_get("room", "sda"), scl = hub_plugin_get("room", "scl");
    s_every_s = hub_plugin_get("room", "every");
    s_sda = sda;
    s_scl = scl;
    if (want == s_run_room_on && (!want || (sda == s_run_sda && scl == s_run_scl))) {
        return;                                     /* 켜짐·핀 그대로면 온습도 태스크는 안 건드림 */
    }
    s_room_gen++;
    for (int i = 0; i < 400 && s_room_alive; i++) {
        vTaskDelay(pdMS_TO_TICKS(25));              /* 최대 10초: room_task 는 1초 주기(센서 읽기가 길어도) */
    }
    if (s_room_alive) {                             /* 옛 태스크가 아직 버스를 쓰는 중 — 지우면 안 된다 */
        ESP_LOGE(TAG, "온습도계 태스크가 끝나지 않아 다시 적용 못 함(관리 화면에서 다시 저장)");
        return;
    }
    if (s_sht) {                                    /* 옛 room_task 가 끝난 뒤에만 I2C 정리 */
        i2c_master_bus_rm_device(s_sht);
        s_sht = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
    s_run_room_on = want;
    s_run_sda = sda;
    s_run_scl = scl;
    s_room_fault = false;                           /* 켜짐·핀이 바뀌면 처음부터 다시 본다 */
    s_room_fails = 0;
    s_room_at_us = 0;
    if (want) {
        hub_plugin_register_status("room", room_status);
        ESP_LOGI(TAG, "온습도계: SDA %d, SCL %d, %d초마다", sda, scl, s_every_s);
        xTaskCreate(room_task, "hub_room", 3072, (void *)(uintptr_t)s_room_gen, 3, NULL);
    } else {
        ESP_LOGI(TAG, "온습도계 꺼짐");
    }
}

/* 핀·켜짐이 바뀌었을 때(관리 화면): 허브를 재부팅하지 않고, 바뀐 센서만 다시 적용한다.
 * 다른 센서와 보일러·각방 클라우드 중계는 그대로 둔다. */
void hub_sensors_reload(void)
{
    static SemaphoreHandle_t mx;                    /* 다시 적용이 겹치지 않게(태스크·I2C 버스를 둘이 만들거나 지우지 않게) */
    if (!mx) {
        mx = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(mx, portMAX_DELAY);
    gas_apply();
    room_apply();
    xSemaphoreGive(mx);
    s_plug_dirty = true;                            /* 새 플러그인 목록을 플랫폼에 다시 알림 */
}

void hub_sensors_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    gas_load();
    gas_apply();
    room_apply();
    xTaskCreate(announce_task, "hub_announce", 3072, NULL, 3, NULL);
}
