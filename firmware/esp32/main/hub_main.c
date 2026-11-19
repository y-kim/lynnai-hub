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
#include "hub_events.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "hub_zigbee.h"
#include "hub_rinnai.h"
#include "hub_sensors.h"
#include "hub_uplink.h"
#include "hub_lan.h"
#include "hub_plugins.h"
#include "hub_zb_rinnai.h"

static const char *TAG = "TEMPERATURE_SENSOR";

#define ESP_TEMP_SENSOR_MIN_VALUE       (-10) /* Sensor min measured value (degree Celsius) */
#define ESP_TEMP_SENSOR_MAX_VALUE       (80)  /* Sensor max measured value (degree Celsius) */
#define ESP_TEMP_SENSOR_UPDATE_INTERVAL (1)   /* Sensor update interval (seconds) */

static int16_t zb_temperature_to_s16(float temp) { return (int16_t)(temp * 100); }

static void esp_app_temp_sensor_handler(float temperature)
{
    int16_t measured_value = zb_temperature_to_s16(temperature);
    /* Update temperature sensor measured value */
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_zcl_set_attr_value(ESP_ZIGBEE_HA_TEMPERATURE_SENSOR_EP_ID, EZB_ZCL_CLUSTER_ID_TEMPERATURE_MEASUREMENT,
                           EZB_ZCL_CLUSTER_SERVER, EZB_ZCL_ATTR_TEMPERATURE_MEASUREMENT_MEASURED_VALUE_ID,
                           EZB_ZCL_STD_MANUF_CODE, (uint8_t *)&measured_value, false);
    esp_zigbee_lock_release();
}

/* 버튼: 드라이버가 눌린 동안 0.2초마다 다시 부르므로, 여기서 뗄 때까지 기다려 길이를 잰다.
 *   짧게(3초 미만) = 보일러 온보딩, 길게(3초 이상) = Zigbee 가입 시도 + LAN 새 드라이버 페어링(3분) */
static void button_event_handler(switch_driver_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != SWITCH_INV_HANDLE, , TAG, "Invalid switch handle");
    int ms = 0;
    while (gpio_get_level(CONFIG_GPIO_BOOT_ON_DEVKIT) == 0 && ms < 3000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        ms += 50;
    }
    if (ms < 3000) {
        ESP_LOGI(TAG, "버튼 짧게 → 보일러 온보딩");
        hub_onboard_trigger();
        return;
    }
    ESP_LOGI(TAG, "버튼 길게 → Zigbee 가입 시도 + LAN 페어링 3분");
    hub_led_set(HUB_LED_BLUE);
    hub_lan_pairing_open(180);
    hub_zigbee_join_now();
    while (gpio_get_level(CONFIG_GPIO_BOOT_ON_DEVKIT) == 0) {   /* 뗄 때까지(다시 불리지 않게) */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    hub_led_set(HUB_LED_OFF);
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

    switch_driver_config_t config = {
        .gpio_num = CONFIG_GPIO_BOOT_ON_DEVKIT,
        .event_cb = button_event_handler,
    };
    ESP_RETURN_ON_FALSE(switch_driver_init(&config) != SWITCH_INV_HANDLE, ESP_FAIL, TAG, "Failed to initialize switch driver");
    is_inited = true;

    return is_inited ? ESP_OK : ESP_FAIL;
}

static void esp_zigbee_alarm_bdb_commissioning(alarm_timer_arg_t arg)
{
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(arg);
    esp_zigbee_lock_release();
}

/* Zigbee 망 찾기(steering)는 Wi-Fi 설정이 끝난 뒤에만 한다.
 * 찾는 동안 라디오가 Zigbee 채널 16개를 훑어 설정용 AP 가 흔들리고, 폰이 IP 를 못 받는다(실측).
 * 실제 사용 순서도 'Wi-Fi 설정 → SmartThings 에서 기기 추가'다. */
static volatile bool s_steer_allowed;
static uint32_t s_backoff_ms = 10000;       /* 가입 실패 시 재시도 간격(점점 늘림) */
static int64_t s_fast_until_us;             /* 이 시각까지는 10초마다 빠르게 재시도(기기 추가 창) */

/* 지금 바로 Zigbee 가입 시도 + 3분간 10초마다 재시도. 버튼 길게 / GET /api/zigbee/join */
void hub_zigbee_join_now(void)
{
    if (!s_steer_allowed) {
        ESP_LOGW(TAG, "Wi-Fi 설정 전이라 Zigbee 가입 보류");
        return;
    }
    if (!ezb_bdb_is_factory_new()) {
        ESP_LOGI(TAG, "이미 Zigbee 망에 가입돼 있음");
        return;
    }
    s_fast_until_us = esp_timer_get_time() + 180LL * 1000000;
    s_backoff_ms = 10000;
    ESP_LOGI(TAG, "Zigbee 가입 시도 시작(3분간 10초마다)");
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();
}

void hub_zigbee_allow_steering(void)
{
    s_steer_allowed = true;
}

static void esp_zigbee_alarm_steer_gate(alarm_timer_arg_t arg)
{
    if (!s_steer_allowed) {
        alarm_timer_schedule(esp_zigbee_alarm_steer_gate, 0, 5000);   /* 스캔 없이 5초 뒤 다시 확인 */
        return;
    }
    ESP_LOGI(TAG, "Zigbee 망 찾기 시작 (Wi-Fi 설정 완료됨)");
    esp_zigbee_lock_acquire(portMAX_DELAY);
    (void)ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    esp_zigbee_lock_release();
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
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Deferred driver initialization %s", deferred_driver_init() ? "failed" : "successful");
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode", ezb_bdb_is_factory_new() ? "" : " non");
            if (ezb_bdb_is_factory_new()) {
                if (s_steer_allowed) {
                    ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
                } else {
                    ESP_LOGI(TAG, "Zigbee 망 찾기 보류: Wi-Fi 설정 먼저");
                    alarm_timer_schedule(esp_zigbee_alarm_steer_gate, 0, 5000);
                }
            } else {
                ESP_LOGI(TAG, "Device reboot");
            }
        } else {
            ESP_LOGW(TAG, "%s failed with status(0x%02x), please retry", ezb_app_signal_to_string(signal_type), status);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_INITIALIZATION, 1000);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ezb_extpanid_t extended_pan_id;
            ezb_nwk_get_extended_panid(&extended_pan_id);
            ESP_LOGI(TAG, "Joined network successfully: PAN ID(0x%04hx, EXT: 0x%llx), Channel(%d), Short Address(0x%04hx)",
                     ezb_nwk_get_panid(), extended_pan_id.u64, ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            hub_event(EV_ZB_JOINED, NULL, ezb_nwk_get_panid(), ezb_nwk_get_current_channel());
            s_backoff_ms = 10000;
            s_fast_until_us = 0;
            hub_led_set(HUB_LED_GREEN);
        } else {
            /* 채널 스캔은 Wi-Fi AP 를 몇 초씩 비워 보일러가 끊긴다(실측). 실패가 이어지면 간격을 늘린다.
             * 단 가입 시도 창(버튼 길게/API 후 3분)에는 10초마다. */
            bool fast = esp_timer_get_time() < s_fast_until_us;
            uint32_t wait = fast ? 10000 : s_backoff_ms;
            ESP_LOGW(TAG, "Failed to join network with status(0x%02x) — %lu초 뒤 다시", status,
                     (unsigned long)(wait / 1000));
            hub_event(EV_ZB_JOIN_FAIL, NULL, status, (int32_t)wait);
            alarm_timer_schedule(esp_zigbee_alarm_bdb_commissioning, EZB_BDB_MODE_NETWORK_STEERING, wait);
            if (!fast && s_backoff_ms < 600000) {
                s_backoff_ms *= 2;
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
    ESP_ERROR_CHECK(nvs_flash_init());

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
    hub_plugins_load();                     /* 플러그인 켜짐·핀(관리 화면) */
    hub_sensors_start();                    /* 온습도계 + 가스 계량기 */
    hub_rinnai_start();                     /* 중계 MQTT 엿보기 → Zigbee */
    hub_proxy_start();                      /* 보일러 TLS1.1 수신 → 클라우드 중계 */
    hub_heap_monitor_start();
    ESP_LOGI(TAG, "Start ESP Zigbee Stack");
    xTaskCreate(esp_zigbee_stack_main_task, "Zigbee_main", 4096, NULL, 5, NULL);
}
