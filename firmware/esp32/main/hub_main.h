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

#include <stdbool.h>
#include <stdint.h>
#include "hub_radio.h"

bool hub_main_chip_temp(int ch, int16_t *c100);   /* 허브 칩 온도(1/100°C). 못 쟀거나 그 통로(EXPOSE_CH_*)의 노출 설정에서 껐으면 false */

#define ESP_ZIGBEE_HA_TEMPERATURE_SENSOR_EP_ID (10)

#define ESP_ZIGBEE_STORAGE_PARTITION_NAME "zb_nvs"   /* 앱 nvs 와 분리: 가입 정보가 앱 NVS 쓰기로 손상되지 않게 */

#define ESP_MANUFACTURER_NAME "\x06""Lynnai"
#define ESP_MODEL_IDENTIFIER "\x0a""LYNNAI-HUB"

/* 역할: 802.15.4 라디오가 있는 칩(C6)은 엔드 디바이스, 라디오를 따로 붙이는 칩(S3 + H2 등)은 라우터.
 * (Zigbee 라이브러리가 S3 용으로는 코디네이터/라우터만 제공한다. 라디오가 따로면 늘 수신할 수 있어 라우터가 맞기도 하다) */
#if CONFIG_ZB_ZED
#define ESP_ZIGBEE_DEVICE_CONFIG()                      \
    {                                                   \
        .device_type = EZB_NWK_DEVICE_TYPE_END_DEVICE,  \
        .install_code_policy = false,                   \
        .zed_config = {                                 \
            .ed_timeout = EZB_NWK_ED_TIMEOUT_64MIN,     \
            .keep_alive = 4000,                         \
        },                                              \
    }
#else
#define ESP_ZIGBEE_DEVICE_CONFIG()                      \
    {                                                   \
        .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,      \
        .install_code_policy = false,                   \
        .zczr_config = {                                \
            .max_children = 10,                         \
        },                                              \
    }
#endif

#if CONFIG_ZB_RADIO_NATIVE
#define ESP_ZIGBEE_PLATFORM_CONFIG()                                 \
    {                                                                \
        .storage_partition_name = ESP_ZIGBEE_STORAGE_PARTITION_NAME, \
        .radio_config = {                                            \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,              \
        },                                                           \
    }
#else
/* 라디오 칩(RCP: ESP32-C6·H2 에 ot_rcp)을 UART 로 붙인 구성. 핀·속도는 hub_radio.h */
#define ESP_ZIGBEE_PLATFORM_CONFIG()                                 \
    {                                                                \
        .storage_partition_name = ESP_ZIGBEE_STORAGE_PARTITION_NAME, \
        .radio_config = {                                            \
            .radio_mode = ESP_ZIGBEE_RADIO_MODE_UART_RCP,            \
            .radio_uart_config = {                                   \
                .port = HUB_RCP_UART_PORT,                           \
                .uart_config = {                                     \
                    .baud_rate = HUB_RCP_UART_BAUD,                  \
                    .data_bits = UART_DATA_8_BITS,                   \
                    .parity = UART_PARITY_DISABLE,                   \
                    .stop_bits = UART_STOP_BITS_1,                   \
                    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,           \
                    .source_clk = UART_SCLK_DEFAULT,                 \
                },                                                   \
                .rx_pin = HUB_RCP_UART_RX,                           \
                .tx_pin = HUB_RCP_UART_TX,                           \
            },                                                       \
        },                                                           \
    }
#endif

#define ESP_ZIGBEE_DEFAULT_CONFIG()                      \
    {                                                    \
        .device_config = ESP_ZIGBEE_DEVICE_CONFIG(),     \
        .platform_config = ESP_ZIGBEE_PLATFORM_CONFIG(), \
    };
