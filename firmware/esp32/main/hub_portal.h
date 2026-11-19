#pragma once
#include "esp_err.h"
#include "hub_state.h"

/* 설정 포털(:80) 시작. st 는 앱 수명 동안 유효해야 한다(설정 저장 시 갱신됨). */
esp_err_t hub_portal_start(hub_state_t *st);

/* 관리자 세션 쿠키가 맞나(다른 모듈의 상태 바꾸는 요청용). 쿠키는 SameSite=Strict 라 다른 사이트에서 온 요청엔 없다 */
#include "esp_http_server.h"
#include <stdbool.h>
bool hub_portal_authed(httpd_req_t *req);
