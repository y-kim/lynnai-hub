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

#pragma once

/* SmartThings Hub 의 채널을 모르므로 11~26 전 채널을 스캔해서 참여한다 */
#define ESP_ZIGBEE_PRIMARY_CHANNEL_MASK   (0x07FFF800U)
#define ESP_ZIGBEE_SECONDARY_CHANNEL_MASK (0x07FFF800U)

#define ESP_ZIGBEE_HA_TEMPERATURE_SENSOR_EP_ID (10)

#define ESP_ZIGBEE_STORAGE_PARTITION_NAME "nvs"

#define ESP_MANUFACTURER_NAME "\x06""Lynnai"
#define ESP_MODEL_IDENTIFIER "\x0a""LYNNAI-HUB"

#define ESP_ZIGBEE_ZED_CONFIG()                         \
    {                                                   \
        .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,  \
        .install_code_policy = false,                   \
        .zed_config = {                                 \
            .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,     \
            .keep_alive = 4000,                         \
        },                                              \
    }

#if CONFIG_SOC_IEEE802154_SUPPORTED
#define ESP_ZIGBEE_PLATFORM_CONFIG()                                 \
    {                                                                \
        .storage_partition_name = ESP_ZIGBEE_STORAGE_PARTITION_NAME, \
        .radio_config = {                                            \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,              \
        },                                                           \
    }
#else
#warning "The example is not for IEEE 802.15.4-disabled SoC usage, please refer to esp_zigbee_gateway for RCP configuration"
#endif

#define ESP_ZIGBEE_DEFAULT_CONFIG()                      \
    {                                                    \
        .device_config = ESP_ZIGBEE_ZED_CONFIG(),        \
        .platform_config = ESP_ZIGBEE_PLATFORM_CONFIG(), \
    };
