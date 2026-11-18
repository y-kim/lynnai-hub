#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/* proxy 모드 시작: 8883 에서 보일러(TLS 1.1)를 받아 클라우드로 중계한다. */
void hub_proxy_start(void);

/* 포털(:80)에 등록할 GET /key/server 처리기: 클라우드에 다시 물어 답을 돌려준다. */
esp_err_t hub_proxy_bootstrap_handler(httpd_req_t *req);
