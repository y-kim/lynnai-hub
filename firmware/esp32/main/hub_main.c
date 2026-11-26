/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: LicenseRef-Included
 *
 * Zigbee HA_temperature_sensor Example
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 */

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "temp_sensor_driver.h"
#include "switch_driver.h"
#include "alarm_timer.h"

#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "ezbee/core.h"
#include "ezbee/nwk.h"

#include "hub_main.h"
#include "hub_wifi.h"
#include "hub_state.h"
#include "hub_dns.h"
#include "hub_portal.h"
#include "hub_proxy.h"
#include "hub_onboard.h"
#include "hub_led.h"
#include "hub_diag.h"
#include "hub_crypt.h"
#include "hub_events.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "hub_zigbee.h"
#include "hub_rinnai.h"
#include "hub_sensors.h"
#include "hub_tariff.h"
#include "hub_uplink.h"
#include "hub_upstream.h"
#include "hub_lan.h"
#include "hub_plugins.h"
#include "hub_expose.h"
#include "hub_pairs.h"
#include "hub_api.h"
#include "hub_zb_rinnai.h"
#include "hub_store.h"
#include "hub_devs.h"
#include "hub_ota.h"
#include "hub_trace.h"
#include "hub_bbox.h"
#include "esp_mac.h"
#include "nvs.h"
#include <string.h>
#include <time.h>

/* Zigbee 가입 표식(앱 NVS, 검사값 포함). 스택의 가입 정보(zb_nvs)는 스택이 직접 써서 감쌀 수 없으므로,
 * '가입했었다'를 따로 남겨 두고 부팅 때 스택이 초기 상태면 "가입 정보 사라짐"으로 보고한다. */
static void zb_mark_set(void)
{
    nvs_handle_t h;
    if (nvs_open("zbmark", NVS_READWRITE, &h) == ESP_OK) {
        hs_set_u8(h, "joined", 1);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void zb_mark_clear(void)            /* 정상 탈퇴: 다음 부팅의 "가입 정보 사라짐" 경보를 막는다 */
{
    nvs_handle_t h;
    if (nvs_open("zbmark", NVS_READWRITE, &h) == ESP_OK) {
        hs_erase(h, "joined");                 /* 값 + 검사값 */
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool zb_mark_get(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("zbmark", NVS_READWRITE, &h) == ESP_OK) {
        hs_get_u8(h, "zbmark", "joined", &v);
        nvs_close(h);
    }
    return v == 1;
}

/* 가입 정보 분실 기록(앱 NVS "zbmark"/"loss") — 재부팅해도 남는다. 이벤트 기록은 RAM 이라 재부팅하면 사라져서
 * '언제·왜 가입 정보가 없어졌는지'를 나중에 알 수 없었다. 관리 화면과 /api 로 보여 준다. */
typedef struct {
    uint8_t  kind;        /* 1 = 부팅 재가입 도중 탈퇴(분실), 2 = 가동 중 탈퇴(SmartThings 에서 삭제한 것으로 추정) */
    uint8_t  leave_type;
    uint16_t count;       /* 누적 횟수 */
    uint32_t epoch;       /* 벽시계(0 = 시계가 맞기 전에 일어남) */
    uint32_t up_s;        /* 그 부팅에서 몇 초째였나 */
} zb_loss_t;
static bool s_loss_this_boot;

static bool zb_loss_load(zb_loss_t *r)
{
    nvs_handle_t h;
    size_t sz = sizeof(*r);
    bool ok = false;
    if (nvs_open("zbmark", NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, "loss", r, &sz) == ESP_OK && sz == sizeof(*r);
        nvs_close(h);
    }
    return ok;
}

static void zb_loss_save(const zb_loss_t *r)
{
    nvs_handle_t h;
    if (nvs_open("zbmark", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "loss", r, sizeof(*r));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void zb_loss_note(uint8_t kind, uint8_t leave_type)
{
    zb_loss_t r;
    if (!zb_loss_load(&r)) {
        memset(&r, 0, sizeof(r));
    }
    r.kind = kind;
    r.leave_type = leave_type;
    if (r.count < 0xffff) {
        r.count++;
    }
    time_t t = time(NULL);
    r.epoch = t > 1700000000 ? (uint32_t)t : 0;
    r.up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    zb_loss_save(&r);
    s_loss_this_boot = true;
}

bool hub_zigbee_last_loss(int *kind, unsigned *count, long *epoch, long *up_s)
{
    zb_loss_t r;
    if (!zb_loss_load(&r) || !r.count) {
        return false;
    }
    if (s_loss_this_boot && !r.epoch) {          /* 부팅 직후(시계 전)에 일어난 일 — 시계가 맞으면 시각을 채운다 */
        time_t t = time(NULL);
        if (t > 1700000000) {
            r.epoch = (uint32_t)(t - (time_t)(esp_timer_get_time() / 1000000 - r.up_s));
            zb_loss_save(&r);
        }
    }
    if (kind) *kind = r.kind;
    if (count) *count = r.count;
    if (epoch) *epoch = (long)r.epoch;
    if (up_s) *up_s = (long)r.up_s;
    return true;
}

/* 가입 데이터를 되살려 재부팅한 횟수(앱 NVS "zbmark"/"rjtry"). 망에 다시 붙으면 0. 재부팅을 넘겨 세야
 * 끝없이 재시작하지 않는다(코디네이터가 꺼져 있거나 SmartThings 에서 정말 지운 경우). */
static uint8_t rjtry_get(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("zbmark", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "rjtry", &v);
        nvs_close(h);
    }
    return v;
}

static void rjtry_set(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("zbmark", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "rjtry", v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static const char *TAG = "TEMPERATURE_SENSOR";

#define ESP_TEMP_SENSOR_MIN_VALUE       (-10) /* Sensor min measured value (degree Celsius) */
#define ESP_TEMP_SENSOR_MAX_VALUE       (80)  /* Sensor max measured value (degree Celsius) */
#define ESP_TEMP_SENSOR_UPDATE_INTERVAL (1)   /* Sensor update interval (seconds) */

static int16_t zb_temperature_to_s16(float temp) { return (int16_t)(temp * 100); }

/* 마지막으로 잰 칩 온도(1/100°C). 생존 신호에 실어 보낸다 — ZCL 속성 보고는 SmartThings 쪽 바인딩이 있어야 가는데,
 * 가입 정보를 되살리거나 다시 가입하면 바인딩이 사라져 온도가 멈춘다(2026-11-21 이후 실제로 멈춰 있었음). */
static volatile int16_t s_chip_c100 = INT16_MIN;

bool hub_main_chip_temp(int ch, int16_t *c100)
{
    int16_t v = s_chip_c100;
    if (v == INT16_MIN || !hub_expose_bit_on(ch, EXPOSE_HUB_TEMP)) {
        return false;                    /* 아직 못 쟀거나 관리 화면에서 그 통로의 허브 칩 온도를 끔 */
    }
    *c100 = v;
    return true;
}

static void esp_app_temp_sensor_handler(float temperature)
{
    s_chip_c100 = zb_temperature_to_s16(temperature);
    if (!hub_expose_bit_on(EXPOSE_CH_ZIGBEE, EXPOSE_HUB_TEMP)) {
        return;                          /* 관리 화면에서 허브 칩 온도를 끔 → 값을 안 바꿔 보고도 멈춤 */
    }
    int16_t measured_value = zb_temperature_to_s16(temperature);
    /* Update temperature sensor measured value */
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(ESP_ZIGBEE_HA_TEMPERATURE_SENSOR_EP_ID, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
                           EZB_ZCL_CLUSTER_SERVER, EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID,
                           EZB_ZCL_STD_MANUF_CODE, (uint8_t *)&measured_value, false);
    esp_zigbee_lock_release();
}

static volatile bool s_lan_pair_after;      /* Zigbee 가입 세션이 끝나면 LAN 페어링 창을 연다(버튼) */

/* 버튼: 드라이버가 눌린 동안 0.2초마다 다시 부르므로, 여기서 뗄 때까지 기다려 길이를 잰다.
 *   짧게(3초 미만) = 스마트홈 허브와 붙기: (Zigbee 를 쓰고 미가입이면) 가입 시도(3분), (LAN 을 쓰면) 새 드라이버
 *                   페어링 3분 — 가입 시도 중엔 Wi-Fi 가 멈추므로 페어링 창은 가입이 끝난 뒤 연다
 *   길게(3초 이상) = 새 기기 Wi-Fi 페어링: 보일러/온도 조절기를 허브 AP 에 등록(온보딩)
 * 주의: USB-UART 의 DTR 이 이 버튼(GPIO9)에 물려 있어, 시리얼 포트를 열 때 '짧게'로 눌릴 수 있다. */
static void button_event_handler(switch_driver_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != SWITCH_INV_HANDLE, , TAG, "Invalid switch handle");
    int ms = 0;
    while (gpio_get_level(CONFIG_GPIO_BOOT_ON_DEVKIT) == 0 && ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        ms += 50;
    }
    if (ms < 3000 && hub_crypt_button()) {          /* 관리 화면에서 암호화 켜기를 고른 뒤의 확인 버튼 */
        return;
    }
    if (ms < 3000) {
        bool lan = hub_uplink_mask() & HUB_UPLINK_LAN;
        ESP_LOGI(TAG, "버튼 짧게 → %s%s", (hub_uplink_mask() & HUB_UPLINK_ZIGBEE) ? "Zigbee 가입 시도 " : "",
                 lan ? "+ LAN 페어링 3분" : "");
        hub_led_set(HUB_LED_BLUE);                 /* 눌림 확인용 짧은 파랑 */
        vTaskDelay(pdMS_TO_TICKS(300));
        hub_led_set(HUB_LED_OFF);
        if (hub_zigbee_join_now()) {
            s_lan_pair_after = lan;                /* 가입 중엔 Wi-Fi 가 멈추므로 LAN 페어링 창은 가입이 끝난 뒤 연다 */
        } else if (lan) {
            hub_lan_pairing_open(180);
        }
        return;
    }
    ESP_LOGI(TAG, "버튼 길게 → 새 기기 Wi-Fi 페어링(온보딩)");
    while (gpio_get_level(CONFIG_GPIO_BOOT_ON_DEVKIT) == 0) {   /* 뗄 때까지(다시 불리지 않게) */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    hub_onboard_trigger();                         /* 온보딩이 LED 를 직접 바꾼다(파랑=검색, 노랑=전송, 초록/빨강=결과) */
}

static esp_err_t deferred_driver_init(void)
{
    static bool is_inited = false;

    ESP_RETURN_ON_FALSE(!is_inited, ESP_OK, TAG, "Deferred driver already initialized");

    temperature_sensor_config_t temp_sensor_config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(ESP_TEMP_SENSOR_MIN_VALUE, ESP_TEMP_SENSOR_MAX_VALUE);
    ESP_RETURN_ON_ERROR(
        temp_sensor_driver_init(&temp_sensor_config, ESP_TEMP_SENSOR_UPDATE_INTERVAL, esp_app_temp_sensor_handler), TAG,
        "Failed to initialize temperature sensor");

    is_inited = true;                    /* 버튼은 app_main 에서(Zigbee 초기화가 실패해도 온보딩·페어링은 되게) */

    return is_inited ? ESP_OK : ESP_FAIL;
}

static void zb_join_session_end(void);

static void esp_zigbee_alarm_bdb_commissioning(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    bool joined = ezb_bdb_dev_joined();
    if (!joined) {
        (void)ezb_bdb_start_top_level_commissioning(arg);
    }
    esp_zigbee_lock_release();
    if (joined) {                                    /* 그 사이 가입됨(버튼 등) — 남은 재시도 타이머는 무시 */
        zb_join_session_end();
    }
}

/* Zigbee 망 찾기(steering)는 Wi-Fi 설정이 끝난 뒤에만 한다.
 * 찾는 동안 라디오가 Zigbee 채널 16개를 훑어 설정용 AP 가 흔들리고, 폰이 IP 를 못 받는다(실측).
 * 실제 사용 순서도 'Wi-Fi 설정 → SmartThings 에서 기기 추가'다. */
bool g_zb_nvs_was_reset = false;            /* 이번 부팅에 zb_nvs 를 지웠나(/api/stats 로 보임) */
static volatile bool s_steer_allowed;
static volatile bool s_ap_busy;             /* 허브 AP 에 기기 등록(온보딩) 중 → Zigbee 가입 보류 */
static volatile bool s_join_session;        /* Wi-Fi 를 멈추고 Zigbee 가입(steering) 진행 중 */
static uint32_t s_backoff_ms = 10000;       /* 가입 실패 시 재시도 간격(점점 늘림) */
static int64_t s_fast_until_us;             /* 이 시각까지는 빠르게 재시도(기기 추가 창) */

/* 마지막 가입 실패 기록 — 관리화면·/api 로 '왜 못 붙었는지' 보여 주려고(선 없이 진단) */
static volatile uint8_t  s_zbf_status;
static volatile uint32_t s_zbf_count;
static volatile int64_t  s_zbf_us;
static volatile uint8_t  s_zbl_type = 0xff; /* 마지막 LEAVE(망에서 나감) 종류, 0xff=없음 */
static volatile int64_t  s_zbl_us;

/* 이번 부팅에 실제로 망에 붙은 적이 있나. 붙기 전(부팅 재가입 도중)의 탈퇴는 "삭제"가 아니라 "분실"이다. */
static volatile bool s_zb_up;
/* 가입 정보가 없을 때(공장 초기) 자동 가입 시도 횟수 한도. SmartThings 에서 '기기 추가'를 열기 전에는 성공할 수 없고
 * 시도할 때마다 Wi-Fi 가 5초쯤 멈춰 린나이 기기가 허브 AP 에서 떨어진다 — 부팅 뒤 10분쯤(6번)만 하고 멈춘다.
 * 그 뒤엔 허브 버튼 짧게 / 관리 화면 'Zigbee 가입 시도'로만(3분 창). */
#define ZB_AUTO_STEER_MAX 6
static volatile uint8_t s_auto_left = ZB_AUTO_STEER_MAX;
static volatile bool s_auto_stopped;
bool hub_zigbee_auto_stopped(void) { return s_auto_stopped; }

/* ---- 재가입 실패로 지워진 가입 데이터 되살리기(espressif/esp-zigbee-sdk#939 우회) ----
 * esp-zigbee-lib 2.1.0 은 재가입이 안 되면(응답을 못 받아 비보안 재가입으로 넘어가고, 키를 못 받으면) 스스로
 * 완전 탈퇴하며 가입 데이터를 지운다. 시판 기기처럼 "가입 정보를 쥔 채 다시 시도"하도록: 지워진 항목을 스냅샷에서
 * 되쓰고 재시작해 재가입을 다시 한다. 스택의 RAM 상태는 이미 초기화돼 있어 재시작이 필요하다.
 * 최대 ZB_RESTORE_MAX 번(간격을 늘려 가며). 그래도 안 되면 그대로 두고 '기기 추가' 안내를 띄운다.
 * 붙어서 돌던 중의 탈퇴(SmartThings 에서 삭제)는 되살리지 않는다. */
#define ZB_RESTORE_MAX 3
static const uint32_t ZB_RESTORE_WAIT_MS[ZB_RESTORE_MAX] = { 5000, 60000, 300000 };
static volatile bool s_restore_pending;
static volatile int64_t s_restore_at_us;

static void zb_restore_task(void *arg)
{
    uint32_t wait_ms = (uint32_t)(uintptr_t)arg;
    int n = hub_crypt_zb_restore_missing();
    ESP_LOGW(TAG, "Zigbee 가입 데이터 %d개 되살림 — %lu초 뒤 재시작해 재가입을 다시 시도", n, (unsigned long)(wait_ms / 1000));
    vTaskDelay(pdMS_TO_TICKS(wait_ms));
    hub_crypt_zb_restore_missing();                 /* 기다리는 사이 더 지워진 게 있으면 마저 */
    hub_ota_planned_reboot();                       /* 의도한 재시작 — 새 펌웨어 시험 부팅 실패로 세지 않게 */
    esp_restart();
}

/* 망에 붙은 뒤 조금 있다가 스냅샷을 새로 뜬다(스택이 저장을 끝낸 뒤). 다음 재가입 실패에 쓸 최신본. */
static void zb_snap_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    int n = hub_crypt_zb_snapshot();
    ESP_LOGI(TAG, "Zigbee 가입 데이터 스냅샷 %d개", n);
    vTaskDelete(NULL);
}

static void zb_up_mark(void)                        /* 망에 붙음(부팅 재가입 성공 / 새 가입 성공) */
{
    hub_bbox_zb_joined();
    s_zb_up = true;
    s_auto_stopped = false;
    if (rjtry_get()) {
        rjtry_set(0);
    }
    xTaskCreate(zb_snap_task, "zb_snap", 4096, NULL, 3, NULL);
}

/* 되살리기 상태: 지금까지 재시도 횟수, 한도, 재시작까지 남은 초(-1 = 예정 없음). 안 쓰는 값은 NULL. */
void hub_zigbee_restore_state(int *tries, int *max, long *reboot_in_s)
{
    if (tries) *tries = rjtry_get();
    if (max) *max = ZB_RESTORE_MAX;
    if (reboot_in_s) {
        int64_t left = s_restore_at_us - esp_timer_get_time();
        *reboot_in_s = s_restore_pending ? (long)(left > 0 ? left / 1000000 : 0) : -1;
    }
}

/* 가입했던 허브가 지금 가입 정보 없이 떠 있나(누가 지운 게 아닌 분실). 저장 데이터 오류가 아니라 '다시 붙이기'가
 * 필요한 상태라, 관리 화면에 따로 안내한다(USB 점검 안내가 아니라 SmartThings 에서 기기 추가). */
bool hub_zigbee_lost(void)
{
    if (!(hub_uplink_mask() & HUB_UPLINK_ZIGBEE) || !hub_zigbee_is_ready()) {
        return false;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    bool lost = !ezb_bdb_dev_joined() && ezb_bdb_is_factory_new();
    esp_zigbee_lock_release();
    return lost && zb_mark_get();
}

static void zb_note_fail(uint8_t status)
{
    hub_bbox_zb_fail(status);
    s_zbf_status = status;
    s_zbf_us = esp_timer_get_time();
    s_zbf_count++;
}

void hub_zigbee_last_fail(uint8_t *status, uint32_t *count, long *age_s,
                          uint8_t *leave_type, long *leave_age_s)
{
    int64_t now = esp_timer_get_time();
    if (status) *status = s_zbf_status;
    if (count) *count = s_zbf_count;
    if (age_s) *age_s = s_zbf_count ? (long)((now - s_zbf_us) / 1000000) : -1;
    if (leave_type) *leave_type = s_zbl_type;
    if (leave_age_s) *leave_age_s = s_zbl_type != 0xff ? (long)((now - s_zbl_us) / 1000000) : -1;
}

const char *hub_zigbee_fail_text(uint8_t status)
{
    switch (status) {
    case EZB_BDB_STATUS_SUCCESS:                  return "성공";
    case EZB_BDB_STATUS_IN_PROGRESS:              return "진행 중";
    case EZB_BDB_STATUS_NO_NETWORK:               return "받아 주는 망을 못 찾음 (SmartThings 에서 '기기 추가'가 안 열렸거나 신호 약함)";
    case EZB_BDB_STATUS_TARGET_FAILURE:           return "가입 요청에 응답 없음 (허브 신호 약함/거리)";
    case EZB_BDB_STATUS_FORMATION_FAILURE:        return "망 구성 실패";
    case EZB_BDB_STATUS_NO_SCAN_RESPONSE:         return "스캔 응답 없음";
    case EZB_BDB_STATUS_NOT_PERMITTED:            return "허용되지 않음";
    case EZB_BDB_STATUS_TCLK_EX_FAILURE:          return "보안키 교환 실패 (Trust Center) — 기존 등록을 지우고 다시 추가 필요할 수 있음";
    case EZB_BDB_STATUS_NOT_ON_A_NETWORK:         return "망에 올라가 있지 않음";
    case EZB_BDB_STATUS_ON_A_NETWORK:             return "이미 망에 가입됨";
    case EZB_BDB_STATUS_CANCELLED:                return "취소됨 (Wi-Fi/온보딩에 라디오 양보)";
    case EZB_BDB_STATUS_DEV_ANNCE_SEND_FAILURE:   return "부모 라우터가 응답(ACK) 없음 — 링크 끊김/신호 약함";
    default:                                      return "알 수 없는 사유";
    }
}

static void esp_zigbee_alarm_steer_gate(alarm_timer_arg_t arg);

/* 재시도 타이머는 종류별로 하나만 걸어 둔다. alarm_timer_schedule 은 부를 때마다 새 타이머를 만들어서,
 * 여러 곳에서 예약하면 같은 일이 겹쳐 돌았다(가입·재가입 시도가 동시에 시작됨). 콜백이 들어오면서 푼다. */
static volatile bool s_arm_steer, s_arm_rejoin, s_arm_reinit;
static void arm_once(volatile bool *armed, alarm_timer_callback_t cb, uint32_t ms)
{
    if (*armed) {
        return;
    }
    if (alarm_timer_schedule(cb, 0, ms) == ESP_OK) {
        *armed = true;
    }
}

/* 2.4GHz 라디오는 Wi-Fi 와 Zigbee 가 공유한다. steering(망 찾기)은 채널 16개를 훑어 라디오를
 * 오래 잡으므로, 그동안 Wi-Fi 를 멈춰 라디오를 Zigbee 에 내준다(가입 세션). 반대로 허브 AP 에
 * 기기를 등록(온보딩)하는 중이면 steering 을 미뤄 라디오를 Wi-Fi 에 양보한다.
 * 모든 가입 경로가 이 함수로 모인다. 타이머/일반 태스크 컨텍스트에서만 호출(stack lock 잡음). */
static void zb_try_steer(void)
{
    if (s_restore_pending) {                         /* 가입 데이터를 되살려 재시작을 기다리는 중 */
        return;
    }
    if (!s_steer_allowed || s_ap_busy) {
        ESP_LOGI(TAG, "Zigbee 가입 보류(%s) — 5초 뒤 다시",
                 s_ap_busy ? "AP 온보딩 중, 라디오 양보" : "Wi-Fi 설정 전");
        arm_once(&s_arm_steer, esp_zigbee_alarm_steer_gate, 5000);
        return;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    bool joined = ezb_bdb_dev_joined();
    esp_zigbee_lock_release();
    if (joined) {                                   /* 남아 있던 타이머가 가입된 뒤에 불림 — Wi-Fi 를 멈추지 않는다 */
        zb_join_session_end();
        return;
    }
    if (!s_join_session) {
        s_join_session = true;
        hub_wifi_radio_pause();                     /* 가입하는 동안 Wi-Fi 중단 */
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    /* 가입 정보가 남아 있으면 '새 망 찾기'가 아니라 '기존 망 재가입'이다. 새 망 찾기는 SmartThings 가 기기 추가를
     * 열어야만 되고, 가입 정보가 있는 기기에 쓸 절차가 아니다. 결과는 DEVICE_REBOOT 신호로 온다(실패하면 간격을
     * 늘려 다시 — esp_zigbee_alarm_reinit). */
    bool fresh = ezb_bdb_is_factory_new();
    if (!fresh) {
        s_zb_up = false;                             /* 망에서 떨어진 상태 — 이 뒤의 탈퇴는 '삭제'가 아니라 '분실' */
    }
    (void)ezb_bdb_start_top_level_commissioning(fresh ? EZB_BDB_MODE_NETWORK_STEERING : EZB_BDB_MODE_INITIALIZATION);
    esp_zigbee_lock_release();
}

/* 가입 세션 종료: Wi-Fi 다시 켠다. */
static void zb_join_session_end(void)
{
    if (s_join_session) {
        s_join_session = false;
        hub_wifi_radio_resume();
    }
    if (s_lan_pair_after) {
        s_lan_pair_after = false;
        hub_lan_pairing_open(180);                  /* 버튼으로 미뤄 둔 LAN 페어링 창(Wi-Fi 가 돌아온 뒤) */
    }
}

/* 지금 바로 Zigbee 가입 시도 + 3분간 빠르게 재시도(그동안 Wi-Fi 중단). 버튼 짧게 / POST /api/zigbee/join */
/* Zigbee 초기화(재부팅 재가입) 재시도 — Wi-Fi 를 멈춘 세션 안에서, 간격을 늘려 가며(핸들러가 예약). */
static uint32_t s_init_backoff_ms = 30000;
static void esp_zigbee_alarm_reinit(alarm_timer_arg_t arg)
{
    (void)arg;
    s_arm_reinit = false;
    if (s_restore_pending) {                         /* 가입 데이터를 되살려 재시작을 기다리는 중 — 스택을 건드리지 않는다 */
        return;
    }
    if (s_ap_busy) {                                 /* AP 기기 등록 중이면 양보 */
        arm_once(&s_arm_reinit, esp_zigbee_alarm_reinit, 5000);
        return;
    }
    if (!s_join_session) {
        s_join_session = true;
        hub_wifi_radio_pause();
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
    esp_zigbee_lock_release();
}

/* 지금 가입 시도를 시작할 수 있나(hub_zigbee_join_now 가 true 를 돌려줄 조건). 응답을 먼저 보내고 시작하려는 호출부용 */
bool hub_zigbee_can_join(void)
{
    if (!(hub_uplink_mask() & HUB_UPLINK_ZIGBEE) || !hub_zigbee_is_ready() || !s_steer_allowed) {
        return false;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    bool joined = ezb_bdb_dev_joined();
    esp_zigbee_lock_release();
    return !joined;
}

bool hub_zigbee_join_now(void)
{
    if (!(hub_uplink_mask() & HUB_UPLINK_ZIGBEE) || !hub_zigbee_is_ready()) {
        ESP_LOGI(TAG, "Zigbee 를 쓰지 않거나 아직 준비 전 — 가입 시도 안 함");
        return false;
    }
    if (!s_steer_allowed) {
        ESP_LOGW(TAG, "Wi-Fi 설정 전이라 Zigbee 가입 보류");
        return false;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    bool joined = ezb_bdb_dev_joined();
    esp_zigbee_lock_release();
    if (joined) {                       /* 실제로 붙어 있을 때만 생략(고아 상태면 재가입 진행) */
        ESP_LOGI(TAG, "이미 Zigbee 망에 가입돼 있음");
        return false;
    }
    s_fast_until_us = esp_timer_get_time() + 180LL * 1000000;
    s_backoff_ms = 10000;
    s_auto_left = 0;                    /* 사람이 연 3분 창이 끝나면 다시 자동으로 돌지 않는다 */
    s_auto_stopped = false;
    ESP_LOGI(TAG, "Zigbee 가입 시도 시작(3분간, 그동안 Wi-Fi 잠깐 중단)");
    zb_try_steer();
    return true;
}

void hub_zigbee_allow_steering(void)
{
    s_steer_allowed = true;
}

/* 허브 AP 에 기기 등록(온보딩) 시작/종료 알림(hub_wifi 에서 호출).
 * 반대 방향 보호: 등록 중이면 Zigbee 가입을 멈추고 라디오를 Wi-Fi 에 양보한다. */
void hub_zigbee_ap_onboarding(bool busy)
{
    s_ap_busy = busy;
    if (busy && s_join_session) {
        ESP_LOGI(TAG, "AP 기기 등록 시작 — Zigbee 가입 멈추고 Wi-Fi 양보");
        zb_join_session_end();
    }
}

/* 주기 워치독: 실제로 망에 안 붙어 있으면(부모가 밀어낸 고아 포함) 재가입을 시도한다.
 * 붙어 있으면 아무 스캔도 안 하고 5분 뒤 다시 확인. */
static void esp_zigbee_alarm_rejoin_check(alarm_timer_arg_t arg)
{
    s_arm_rejoin = false;
    if (!s_steer_allowed) {
        arm_once(&s_arm_rejoin, esp_zigbee_alarm_rejoin_check, 300000);
        return;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    bool joined = ezb_bdb_dev_joined();
    esp_zigbee_lock_release();
    if (joined) {
        s_zb_up = true;
        arm_once(&s_arm_rejoin, esp_zigbee_alarm_rejoin_check, 300000);
    } else {
        ESP_LOGW(TAG, "Zigbee 미가입 감지 — 재가입(rejoin) 시도");
        zb_try_steer();
    }
}

static void esp_zigbee_alarm_steer_gate(alarm_timer_arg_t arg)
{
    (void)arg;
    s_arm_steer = false;
    zb_try_steer();
}

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    /* 부팅 기록: 신호마다 시각·프레임 카운터를 남긴다(재가입이 어디서 틀어지는지 순서대로 보려고) */
    hub_bbox_note("sig %s(0x%02x) fc=%lu joined=%d fnew=%d", ezb_app_signal_to_string(signal_type), (unsigned)signal_type,
                  (unsigned long)ezb_nwk_get_frame_counter(), ezb_bdb_dev_joined(), ezb_bdb_is_factory_new());

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        hub_bbox_zb_boot(ezb_nwk_get_frame_counter());
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        hub_wifi_sta_release();             /* 부팅 재가입 결과가 나왔다 — 미뤄 둔 집 Wi-Fi 접속 시작 */
        zb_join_session_end();              /* 재초기화 세션이었다면 Wi-Fi 다시 켬 */
        if (status == EZB_BDB_STATUS_SUCCESS) {
            s_init_backoff_ms = 30000;
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new() && zb_mark_get()) {      /* 가입했었는데 초기 상태 — 관리 화면 맨 위 경고(hub_zigbee_lost) */
                ESP_LOGE(TAG, "Zigbee 가입 정보가 사라져 있음 — SmartThings 에서 '기기 추가' 필요");
            }
            if (ezb_bdb_is_factory_new()) {
                /* 가입은 타이머 컨텍스트에서(Wi-Fi 중단·AP 온보딩 양보·stack lock 포함) */
                arm_once(&s_arm_steer, esp_zigbee_alarm_steer_gate, 100);
            } else {
                ESP_LOGI(TAG, "Device reboot");
                zb_up_mark();                    /* 저장된 가입 정보로 망에 다시 붙음 */
                arm_once(&s_arm_rejoin, esp_zigbee_alarm_rejoin_check, 20000);   /* 고아면 자동 재가입 */
            }
        } else {
            /* 망을 못 찾음(0x03 등). 예제처럼 1초마다 다시 초기화하면 재가입 스캔(~11초)이 라디오를 쉬지 않고 잡아
             * 집 Wi-Fi 가 AP 를 못 찾고(사유 201) 허브가 집 네트워크에서 사라진다(실측). → 간격을 늘려가며,
             * 재시도는 Wi-Fi 를 잠깐 멈춘 세션 안에서. 그 사이엔 Wi-Fi 가 붙어 보일러 중계·관리 화면이 산다. */
            ESP_LOGW(TAG, "%s failed with status(0x%02x) — %lu초 뒤 다시(그동안 Wi-Fi 우선)",
                     ezb_app_signal_to_string(signal_type), status, (unsigned long)(s_init_backoff_ms / 1000));
            zb_note_fail(status);               /* 부팅/재초기화 가입 실패도 사유 기록 */
            arm_once(&s_arm_reinit, esp_zigbee_alarm_reinit, s_init_backoff_ms);
            if (s_init_backoff_ms < 600000) {
                s_init_backoff_ms *= 2;
            }
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            zb_mark_set();                  /* 다음 부팅에 가입 정보가 사라졌는지 알 수 있게 */
            zb_up_mark();
            ESP_LOGI(TAG, "Joined network successfully: PAN ID(0x%04hx, EXT: 0x%llx), Channel(%d), Short Address(0x%04hx)",
                     ezb_nwk_get_panid(), extended_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            hub_event(EV_ZB_JOINED, NULL, ezb_nwk_get_panid(), ezb_nwk_get_current_channel());
            s_backoff_ms = 10000;
            s_fast_until_us = 0;
            zb_join_session_end();              /* 붙었으니 Wi-Fi 다시 켠다 */
            hub_led_set(HUB_LED_GREEN);
            arm_once(&s_arm_rejoin, esp_zigbee_alarm_rejoin_check, 300000);
        } else {
            bool fast = esp_timer_get_time() < s_fast_until_us;
            hub_event(EV_ZB_JOIN_FAIL, NULL, status, 0);
            zb_note_fail(status);               /* 관리화면·/api 에 사유 표시 */
            if (fast) {
                /* 가입 창: Wi-Fi 는 계속 멈춰 둔 채(라디오 전부 Zigbee) 바로 다시 시도 */
                ESP_LOGW(TAG, "Failed to join (0x%02x) — Wi-Fi 멈춘 채 8초 뒤 재시도", status);
                alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_STEERING, 8000);
            } else {
                /* 배경 자가복구: Wi-Fi 다시 켜고 점점 긴 간격으로 재시도(그때 또 잠깐 멈춤) */
                zb_join_session_end();
                if (ezb_bdb_is_factory_new() && s_auto_left == 0) {
                    /* 가입 정보가 없으면 '기기 추가'가 열려야만 붙는다 — 끝없이 시도하며 Wi-Fi 를 끊지 않는다 */
                    s_auto_stopped = true;
                    ESP_LOGW(TAG, "Failed to join (0x%02x) — 자동 가입 시도 중단. 허브 버튼 짧게 또는 관리 화면 "
                                  "'Zigbee 가입 시도'로 다시 시작", status);
                } else {
                    if (s_auto_left) {
                        s_auto_left--;
                    }
                    ESP_LOGW(TAG, "Failed to join (0x%02x) — Wi-Fi 복귀, %lu초 뒤 재가입", status,
                             (unsigned long)(s_backoff_ms / 1000));
                    arm_once(&s_arm_steer, esp_zigbee_alarm_steer_gate, s_backoff_ms);
                    if (s_backoff_ms < 600000) {
                        s_backoff_ms *= 2;
                    }
                }
            }
        }
    } break;
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *dev_annce_params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "New device commissioned or rejoined (short: 0x%04hx)", dev_annce_params->short_addr);
    } break;
    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *leave_params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Left network successfully with type(0x%02x)", leave_params->leave_type);
        hub_event(EV_ZB_LEAVE, NULL, leave_params->leave_type, 0);
        s_zbl_type = leave_params->leave_type;
        s_zbl_us = esp_timer_get_time();
        hub_bbox_zb_left(leave_params->leave_type, s_zb_up);
        if (leave_params->leave_type == EZB_ZDO_LEAVE_TYPE_RESET) {     /* 가입 정보가 지워지는 탈퇴 */
            if (!s_zb_up) {
                /* 이번 부팅에 망에 붙기도 전에 탈퇴 = 부팅 재가입이 실패해 가입 정보를 잃은 것. 누가 지운 게 아니므로
                 * 표식을 남겨 경보가 뜨게 하고(다음 부팅에도), 언제 일어났는지 플래시에 적는다. */
                zb_loss_note(1, leave_params->leave_type);
                uint8_t tries = rjtry_get();
                if (hub_crypt_zb_snapshot_joined() && tries < ZB_RESTORE_MAX && !s_restore_pending) {
                    uint32_t wait = ZB_RESTORE_WAIT_MS[tries];
                    rjtry_set(tries + 1);
                    s_restore_at_us = esp_timer_get_time() + (int64_t)wait * 1000;
                    s_restore_pending = true;
                    ESP_LOGE(TAG, "재가입 실패로 스택이 가입 정보를 지움 — 되살려 다시 시도 (%d/%d)", tries + 1, ZB_RESTORE_MAX);
                    if (xTaskCreate(zb_restore_task, "zb_restore", 4096, (void *)(uintptr_t)wait, 5, NULL) != pdPASS) {
                        s_restore_pending = false;
                    }
                } else {
                    ESP_LOGE(TAG, "재가입 실패로 Zigbee 가입 정보가 사라짐 — SmartThings 에서 '기기 추가' 필요");
                }
            } else {
                /* 붙어서 돌던 중의 탈퇴 = SmartThings 에서 기기를 지운 경우(정상). 사라짐 경보는 내지 않는다 */
                zb_loss_note(2, leave_params->leave_type);
                zb_mark_clear();
            }
            s_zb_up = false;
        }
    } break;
    case EZB_ZDO_SIGNAL_PARENT_LOST:
        /* 부모와 연락이 끊김. 이 뒤에 오는 탈퇴는 누가 지운 게 아니라 재가입 실패로 본다(위 LEAVE 처리) */
        ESP_LOGW(TAG, "Zigbee 부모와 연결이 끊김");
        hub_bbox_radio("parent_lost");
        s_zb_up = false;
        break;
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        uint8_t duration = *(uint8_t *)ezb_app_signal_get_params(app_signal);
        if (duration) {
            ESP_LOGI(TAG, "Network(0x%04hx) is open for %d seconds", ezb_nwk_get_panid(), duration);
        } else {
            ESP_LOGW(TAG, "Network(0x%04hx) closed, devices joining not allowed.", ezb_nwk_get_panid());
        }
    } break;
    default:
        ESP_LOGI(TAG, "Zigbee APP Signal: %s(type: 0x%02x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
    return true;
}

static void esp_zigbee_zcl_core_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    switch (callback_id) {
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *default_rsp = (ezb_zcl_cmd_default_rsp_message_t *)message;
        ESP_LOGI(TAG, "Received ZCL Default Response: status(0x%02x)", default_rsp->in.status_code);
    } break;
    default:
        ESP_LOGW(TAG, "ZCL Core Action: ID(0x%04lx)", callback_id);
        break;
    }
}

esp_err_t esp_zigbee_create_zha_temperature_sensor_device(void)
{
    ezb_af_device_desc_t                dev_desc               = ezb_af_create_device_desc();
    ezb_zha_temperature_sensor_config_t temperature_sensor_cfg = EZB_ZHA_TEMPERATURE_SENSOR_CONFIG();
    temperature_sensor_cfg.temp_meas_cfg.min_measured_value    = zb_temperature_to_s16(ESP_TEMP_SENSOR_MIN_VALUE);
    temperature_sensor_cfg.temp_meas_cfg.max_measured_value    = zb_temperature_to_s16(ESP_TEMP_SENSOR_MAX_VALUE);

    ezb_af_ep_desc_t ep_desc =
        ezb_zha_create_temperature_sensor(ESP_ZIGBEE_HA_TEMPERATURE_SENSOR_EP_ID, &temperature_sensor_cfg);
    ezb_zcl_cluster_desc_t basic_desc = {0};

    basic_desc = ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)ESP_MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)ESP_MODEL_IDENTIFIER);

    ESP_ERROR_CHECK(ezb_af_endpoint_add_cluster_desc(ep_desc, hub_zb_rinnai_cluster_desc()));   /* 보일러·각방 */

    ESP_ERROR_CHECK(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc));
    ESP_ERROR_CHECK(ezb_af_device_desc_register(dev_desc));

    ezb_zcl_core_action_handler_register(esp_zigbee_zcl_core_action_handler);

    return ESP_OK;
}

esp_err_t esp_zigbee_setup_commissioning(void)
{
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ESP_ZIGBEE_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ESP_ZIGBEE_SECONDARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(esp_zigbee_app_signal_handler));

    return ESP_OK;
}

static TaskHandle_t s_zb_task;
int hub_zigbee_stack_free(void) { return s_zb_task ? (int)uxTaskGetStackHighWaterMark(s_zb_task) : 0; }

static void esp_zigbee_stack_main_task(void *pvParameters)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ESP_ERROR_CHECK(esp_zigbee_setup_commissioning());

    ESP_ERROR_CHECK(esp_zigbee_create_zha_temperature_sensor_device());

    /* Wi-Fi AP 와 라디오 공존: 평소엔 Zigbee 수신을 끄고(라디오를 Wi-Fi 에 양보) 2초마다 부모에게
     * 쌓인 메시지를 가져온다(폴링). 참여(join) 때 부모에게 알리는 값이라 바꾸면 재참여가 필요하다. */
#if CONFIG_ZB_ZED
    ezb_set_rx_on_when_idle(false);
    ezb_nwk_set_keepalive_interval(2000);
#endif                                    /* 라우터(라디오가 따로인 보드)는 늘 수신한다 — 폴링 설정이 없다 */

    ESP_ERROR_CHECK(esp_zigbee_start(false));
    hub_zigbee_mark_ready();              /* 이제 ezb_* 안전 — 이 전의 센서 전송은 무시됨 */

    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();

    vTaskDelete(NULL);
}

void app_main(void)
{
    hub_led_init();          /* 부팅 직후 바로 LED 끄기(초기 밝은 플래시 최소화) */
    hub_event(EV_BOOT, NULL, esp_reset_reason(), 0);
    /* 저장소 두 개를 연다: 앱 nvs + Zigbee 전용 zb_nvs(zboss 가 가입 정보를 저장, 안 열면 ezb_plat_datasets_init 에서
     * abort). eFuse 에 HMAC 키가 있으면 암호화된 채로 열고, 관리 화면에서 암호화를 켰다면 여기서 옮긴 뒤 재시작한다. */
    {
        bool zb_reset = false;
        ESP_ERROR_CHECK(hub_crypt_boot(&zb_reset));
        if (zb_reset) {
            /* zb_nvs 를 지웠으면 Zigbee 가입 정보가 사라져 재페어링이 필요하다 — 흔적을 남긴다(/api/stats zb_nvs_reset) */
            ESP_LOGW(TAG, "zb_nvs 를 지우고 새로 만듦: Zigbee 가입 정보 사라짐");
            g_zb_nvs_was_reset = true;
            hub_store_report("zb_nvs", "partition", "초기화 실패 → 지움");
        }
    }
    hub_ota_boot_guard();    /* OTA 로 올린 새 펌웨어의 시험 부팅 — 확정 못 하면 이전 칸으로 되돌린다 */
    /* 부팅 기록(GET /api/v1/bootlog): 이 뒤의 로그를 재시작해도 남는 RAM 에 담고, 부팅 이력(플래시)에 이번 부팅을 올린다.
     * 시험 부팅 검사 뒤에 둔다 — 여기서 문제가 생겨도 시험 부팅 횟수에 잡혀 이전 펌웨어로 돌아갈 수 있게. */
    hub_bbox_init();
    hub_bbox_start();
    {
        /* 스택이 손대기 전의 가입 데이터를 RAM 에 떠 둔다 — 부팅 재가입이 실패해 지워지면 되살린다 */
        int n = hub_crypt_zb_snapshot();
        ESP_LOGI(TAG, "Zigbee 가입 데이터 스냅샷 %d개%s", n, hub_crypt_zb_snapshot_joined() ? " (가입 상태)" : "");
    }

    /* 허브: 설정 상태 → Wi-Fi(미설정이면 설정 AP, 설정 후엔 집 Wi-Fi + 전용 AP) → DNS → 설정 포털 */
    static hub_state_t s_hub;
    ESP_ERROR_CHECK(hub_state_load(&s_hub));
    ESP_ERROR_CHECK(hub_wifi_init());
    ESP_ERROR_CHECK(hub_wifi_start(&s_hub));
    hub_dns_start(hub_wifi_ap_ip(), !s_hub.provisioned);
    hub_uplink_load();                      /* Zigbee / LAN (/ Matter) 중 무엇을 쓸지 */
    hub_label_load();                       /* 이 허브 라벨(층 이름) */
    hub_upstream_start();                   /* 멀티허브 설정(모드·라벨·토큰) — Zigbee/ hub_lan 결정 전에 읽는다 */
    /* 위성 허브는 Zigbee 를 안 쓴다(LAN 으로 중심에 붙음). 쓸데없는 Zigbee 가입 시도는 2.4GHz 를 Wi-Fi 와 경합시켜
     * 집 Wi-Fi 를 끊어 놓으므로, 위성이면 steering 을 아예 열지 않는다. */
    if (s_hub.provisioned && hub_upstream_mode() != HUB_UP_GUEST) {
        hub_zigbee_allow_steering();
    }
    hub_onboard_init(&s_hub);
    ESP_ERROR_CHECK(hub_portal_start(&s_hub));
    /* hub_lan(= 멀티허브 위성 다이얼러·중심 수신 서버가 사는 곳)은 ST 통로가 LAN 이거나, 멀티허브 모드가 켜져 있으면 띄운다.
     * (Zigbee 로 ST 에 붙는 중심/위성도 허브끼리는 LAN 으로 통신하므로 반드시 필요) */
    if ((hub_uplink_mask() & HUB_UPLINK_LAN) || hub_upstream_mode() != HUB_UP_OFF) {
        hub_lan_start(s_hub.device_id);     /* SSDP + TCP 8484 */
    }
    hub_api_start(s_hub.device_id);         /* 서버 API 토큰 + 시간 맞추기 */
    hub_plugins_load();                     /* 플러그인 켜짐·핀(관리 화면) */
    hub_expose_load();                      /* 항목별 노출 통로(Zigbee/LAN/Matter) */
    hub_pairs_load();                       /* 보일러↔써머스탯 수동 짝 */
    hub_sensors_start();                    /* 온습도계 + 가스 계량기 */
    hub_tariff_init();                       /* 가스 요금 요소(열량·보정·단가) 중계 → 드라이버 */
    hub_devs_load();                        /* 등록된 린나이 기기 목록(등록 안 된 단말은 기기로 보지 않는다) */
    hub_trace_init();                       /* 기기↔클라우드 메시지 기록(GET /api/v1/trace) */
    hub_rinnai_start();                     /* 중계 MQTT 엿보기 → Zigbee */
    hub_proxy_start();                      /* 보일러 TLS1.1 수신 → 클라우드 중계 */
    hub_heap_monitor_start();
    hub_diag_start(&s_hub);                 /* LED 오류 코드 + USB 시리얼 diag */
    {
        /* 버튼(GPIO9): Zigbee 와 상관없이 — SmartThings 허브가 꺼져 Zigbee 초기화가 실패해도 온보딩·LAN 페어링은 된다 */
        switch_driver_config_t bc = { .gpio_num = CONFIG_GPIO_BOOT_ON_DEVKIT, .event_cb = button_event_handler };
        if (switch_driver_init(&bc) == SWITCH_INV_HANDLE) {
            ESP_LOGE(TAG, "버튼 초기화 실패");
        }
    }
    if (hub_uplink_mask() & HUB_UPLINK_ZIGBEE) {
        ESP_LOGI(TAG, "Start ESP Zigbee Stack");
        /* 4096 이었다 — 부팅 기록이 스택의 디버그 메시지까지 글자로 만들므로(이 태스크 스택에서) 여유를 둔다 */
        xTaskCreate(esp_zigbee_stack_main_task, "Zigbee_main", 6144, NULL, 5, &s_zb_task);
    } else {
        ESP_LOGI(TAG, "Zigbee 를 쓰지 않음(연결 방식) — Zigbee 스택 시작 안 함");
        hub_wifi_sta_release();             /* 기다릴 Zigbee 재가입이 없다 */
    }
}
