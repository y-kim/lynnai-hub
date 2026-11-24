#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

/* 서버(기록·그래프)용 인터페이스: /api/v1/state, /metrics. 형식·인증은 hub_api.c 맨 위. */
void hub_api_start(const char *device_id);     /* 토큰 읽기 + SNTP 시간 맞추기 */
void hub_api_register(httpd_handle_t srv);

/* 읽기 API 인증(토큰이 있으면 Bearer 요구). false 면 401 을 이미 보냈다 */
bool hub_api_authorized(httpd_req_t *req);

/* 지금 난방으로 보일러를 쓰는 중인가(가스 계량기 감시용): 연결된 조절기의 켜진 방 중 '난방 중' 이 하나라도 있고,
 * 보일러 조절기가 있으면 그 전원·난방도 켜져 있을 때. 온수만 쓰는 것은 상태에 표시가 없어 알 수 없다. */
bool hub_api_heating(void);
/* 보일러가 지금 가스를 태우는 중인가(가스 계량기 감시용): 연결된 보일러 조절기 상태의 연소 표시(난방·온수).
 * 비트 1 = 난방 연소, 2 = 온수 연소, 0 = 아님, -1 = 연결된 보일러 조절기가 없어 모른다(그때는 hub_api_heating 으로 본다). */
int hub_api_burning(void);

/* 관리 화면 */
bool hub_api_has_token(void);
bool hub_api_token_new(char out[33]);   /* 저장 실패면 false(토큰 그대로) */
bool hub_api_token_clear(void);
