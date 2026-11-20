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
#include "hub_uplink.h"
#include "hub_lan.h"
#include "hub_plugins.h"
#include "hub_expose.h"
#include "hub_pairs.h"
#include "hub_api.h"
#include "hub_zb_rinnai.h"
#include "hub_store.h"
#include "nvs.h"

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

static const char *TAG = "TEMPERATURE_SENSOR";

#define ESP_TEMP_SENSOR_MIN_VALUE       (-10) /* Sensor min measured value (degree Celsius) */
#define ESP_TEMP_SENSOR_MAX_VALUE       (80)  /* Sensor max measured value (degree Celsius) */
#define ESP_TEMP_SENSOR_UPDATE_INTERVAL (1)   /* Sensor update interval (seconds) */

static int16_t zb_temperature_to_s16(float temp) { return (int16_t)(temp * 100); }

static void esp_app_temp_sensor_handler(float temperature)
{
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

static void esp_zigbee_alarm_steer_gate(alarm_timer_arg_t arg);

/* 2.4GHz 라디오는 Wi-Fi 와 Zigbee 가 공유한다. steering(망 찾기)은 채널 16개를 훑어 라디오를
 * 오래 잡으므로, 그동안 Wi-Fi 를 멈춰 라디오를 Zigbee 에 내준다(가입 세션). 반대로 허브 AP 에
 * 기기를 등록(온보딩)하는 중이면 steering 을 미뤄 라디오를 Wi-Fi 에 양보한다.
 * 모든 가입 경로가 이 함수로 모인다. 타이머/일반 태스크 컨텍스트에서만 호출(stack lock 잡음). */
static void zb_try_steer(void)
{
    if (!s_steer_allowed || s_ap_busy) {
        ESP_LOGI(TAG, "Zigbee 가입 보류(%s) — 5초 뒤 다시",
                 s_ap_busy ? "AP 온보딩 중, 라디오 양보" : "Wi-Fi 설정 전");
        alarm_timer_schedule(esp_zigbee_alarm_steer_gate, 0, 5000);
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
    (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
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
    if (s_ap_busy) {                                 /* AP 기기 등록 중이면 양보 */
        alarm_timer_schedule(esp_zigbee_alarm_reinit, 0, 5000);
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
    if (!s_steer_allowed) {
        alarm_timer_schedule(esp_zigbee_alarm_rejoin_check, 0, 300000);
        return;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    bool joined = ezb_bdb_dev_joined();
    esp_zigbee_lock_release();
    if (joined) {
        alarm_timer_schedule(esp_zigbee_alarm_rejoin_check, 0, 300000);
    } else {
        ESP_LOGW(TAG, "Zigbee 미가입 감지 — 재가입(rejoin) 시도");
        zb_try_steer();
    }
}

static void esp_zigbee_alarm_steer_gate(alarm_timer_arg_t arg)
{
    (void)arg;
    zb_try_steer();
}

static bool esp_zigbee_app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        zb_join_session_end();              /* 재초기화 세션이었다면 Wi-Fi 다시 켬 */
        if (status == EZB_BDB_STATUS_SUCCESS) {
            s_init_backoff_ms = 30000;
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new() && zb_mark_get()) {
                hub_store_report("zigbee", "가입정보", "사라짐 — 재페어링 필요");   /* 가입했었는데 초기 상태 */
            }
            if (ezb_bdb_is_factory_new()) {
                /* 가입은 타이머 컨텍스트에서(Wi-Fi 중단·AP 온보딩 양보·stack lock 포함) */
                alarm_timer_schedule(esp_zigbee_alarm_steer_gate, 0, 100);
            } else {
                ESP_LOGI(TAG, "Device reboot");
                alarm_timer_schedule(esp_zigbee_alarm_rejoin_check, 0, 20000);   /* 고아면 자동 재가입 */
            }
        } else {
            /* 망을 못 찾음(0x03 등). 예제처럼 1초마다 다시 초기화하면 재가입 스캔(~11초)이 라디오를 쉬지 않고 잡아
             * 집 Wi-Fi 가 AP 를 못 찾고(사유 201) 허브가 집 네트워크에서 사라진다(실측). → 간격을 늘려가며,
             * 재시도는 Wi-Fi 를 잠깐 멈춘 세션 안에서. 그 사이엔 Wi-Fi 가 붙어 보일러 중계·관리 화면이 산다. */
            ESP_LOGW(TAG, "%s failed with status(0x%02x) — %lu초 뒤 다시(그동안 Wi-Fi 우선)",
                     ezb_app_signal_to_string(signal_type), status, (unsigned long)(s_init_backoff_ms / 1000));
            alarm_timer_schedule(esp_zigbee_alarm_reinit, 0, s_init_backoff_ms);
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
            ESP_LOGI(TAG, "Joined network successfully: PAN ID(0x%04hx, EXT: 0x%llx), Channel(%d), Short Address(0x%04hx)",
                     ezb_nwk_get_panid(), extended_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            hub_event(EV_ZB_JOINED, NULL, ezb_nwk_get_panid(), ezb_nwk_get_current_channel());
            s_backoff_ms = 10000;
            s_fast_until_us = 0;
            zb_join_session_end();              /* 붙었으니 Wi-Fi 다시 켠다 */
            hub_led_set(HUB_LED_GREEN);
            alarm_timer_schedule(esp_zigbee_alarm_rejoin_check, 0, 300000);
        } else {
            bool fast = esp_timer_get_time() < s_fast_until_us;
            hub_event(EV_ZB_JOIN_FAIL, NULL, status, 0);
            if (fast) {
                /* 가입 창: Wi-Fi 는 계속 멈춰 둔 채(라디오 전부 Zigbee) 바로 다시 시도 */
                ESP_LOGW(TAG, "Failed to join (0x%02x) — Wi-Fi 멈춘 채 8초 뒤 재시도", status);
                alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_STEERING, 8000);
            } else {
                /* 배경 자가복구: Wi-Fi 다시 켜고 점점 긴 간격으로 재시도(그때 또 잠깐 멈춤) */
                zb_join_session_end();
                ESP_LOGW(TAG, "Failed to join (0x%02x) — Wi-Fi 복귀, %lu초 뒤 재가입", status,
                         (unsigned long)(s_backoff_ms / 1000));
                alarm_timer_schedule(esp_zigbee_alarm_steer_gate, 0, s_backoff_ms);
                if (s_backoff_ms < 600000) {
                    s_backoff_ms *= 2;
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
        zb_mark_clear();                        /* 스스로(또는 SmartThings 에서 지워져) 나갔음 — 사라짐 경보 아님 */
    } break;
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

static void esp_zigbee_stack_main_task(void *pvParameters)
{
    esp_zigbee_config_t config = ESP_ZIGBEE_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(esp_zigbee_init(&config));

    ESP_ERROR_CHECK(esp_zigbee_setup_commissioning());

    ESP_ERROR_CHECK(esp_zigbee_create_zha_temperature_sensor_device());

    /* Wi-Fi AP 와 라디오 공존: 평소엔 Zigbee 수신을 끄고(라디오를 Wi-Fi 에 양보) 2초마다 부모에게
     * 쌓인 메시지를 가져온다(폴링). 참여(join) 때 부모에게 알리는 값이라 바꾸면 재참여가 필요하다. */
    ezb_set_rx_on_when_idle(false);
    ezb_nwk_set_keepalive_interval(2000);

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

    /* 허브: 설정 상태 → Wi-Fi(미설정이면 설정 AP, 설정 후엔 집 Wi-Fi + 전용 AP) → DNS → 설정 포털 */
    static hub_state_t s_hub;
    ESP_ERROR_CHECK(hub_state_load(&s_hub));
    ESP_ERROR_CHECK(hub_wifi_init());
    ESP_ERROR_CHECK(hub_wifi_start(&s_hub));
    hub_dns_start(hub_wifi_ap_ip(), !s_hub.provisioned);
    if (s_hub.provisioned) {
        hub_zigbee_allow_steering();
    }
    hub_onboard_init(&s_hub);
    ESP_ERROR_CHECK(hub_portal_start(&s_hub));
    hub_uplink_load();                      /* Zigbee / LAN (/ Matter) 중 무엇을 쓸지 */
    if (hub_uplink_mask() & HUB_UPLINK_LAN) {
        hub_lan_start(s_hub.device_id);     /* SSDP + TCP 8484 */
    }
    hub_api_start(s_hub.device_id);         /* 서버 API 토큰 + 시간 맞추기 */
    hub_plugins_load();                     /* 플러그인 켜짐·핀(관리 화면) */
    hub_expose_load();                      /* 항목별 노출 통로(Zigbee/LAN/Matter) */
    hub_pairs_load();                       /* 보일러↔써머스탯 수동 짝 */
    hub_sensors_start();                    /* 온습도계 + 가스 계량기 */
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
        xTaskCreate(esp_zigbee_stack_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
    } else {
        ESP_LOGI(TAG, "Zigbee 를 쓰지 않음(연결 방식) — Zigbee 스택 시작 안 함");
    }
}
