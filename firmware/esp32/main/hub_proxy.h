#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/*
 * 동작 모드(관리 화면 맨 위, NVS "proxy"/"mode"):
 *   프록시 모드 : 기기 ↔ 허브 ↔ 린나이 클라우드 중계. 린나이 앱 그대로 + SmartThings.
 *   단독 모드   : 클라우드 없이 허브가 기기의 MQTT 상대(브로커)가 된다. 인터넷이 없어도 SmartThings 로 제어,
 *                 린나이 앱으로는 안 보이고 제어도 안 된다.
 * 바꾸면 지금 연결을 끊어 기기가 다시 접속할 때 새 모드로 붙는다.
 */
#define HUB_MODE_PROXY      0
#define HUB_MODE_STANDALONE 1

/* 8883 에서 보일러(TLS 1.1)를 받는다. 모드에 따라 클라우드로 중계하거나 직접 응답한다. */
void hub_proxy_start(void);
uint8_t hub_proxy_mode(void);
void hub_proxy_render(char *out, size_t cap);                     /* 관리 화면 '동작 모드' */
bool hub_proxy_apply(const char *form, char *msg, size_t msglen);   /* POST /admin/mode */

/* 포털(:80)에 등록할 GET /key/server 처리기: 클라우드에 다시 물어 답을 돌려준다(못 하면 마지막 답을 재사용). */
esp_err_t hub_proxy_bootstrap_handler(httpd_req_t *req);
