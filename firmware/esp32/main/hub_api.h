#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

/* 서버(기록·그래프)용 인터페이스: /api/v1/state, /metrics. 형식·인증은 hub_api.c 맨 위. */
void hub_api_start(const char *device_id);     /* 토큰 읽기 + SNTP 시간 맞추기 */
void hub_api_register(httpd_handle_t srv);

/* 읽기 API 인증(토큰이 있으면 Bearer 요구). false 면 401 을 이미 보냈다 */
bool hub_api_authorized(httpd_req_t *req);

/* 관리 화면 */
bool hub_api_has_token(void);
bool hub_api_token_new(char out[33]);   /* 저장 실패면 false(토큰 그대로) */
bool hub_api_token_clear(void);
