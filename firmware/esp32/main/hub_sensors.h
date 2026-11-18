#pragma once
#include "esp_http_server.h"

/* 허브에 직접 꽂는 온습도계(SHT31) + 가스 계량기 펄스. 핀·형식은 hub_sensors.c 맨 위. */
void hub_sensors_start(void);
void hub_sensors_register(httpd_handle_t srv);   /* /api/gas, /api/gas/set */
void hub_sensors_refresh(void);                   /* SmartThings refresh: 가스 값을 곧 다시 보낸다 */
