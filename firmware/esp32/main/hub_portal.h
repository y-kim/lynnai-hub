#pragma once
#include "esp_err.h"
#include "hub_state.h"

/* 설정 포털(:80) 시작. st 는 앱 수명 동안 유효해야 한다(설정 저장 시 갱신됨). */
esp_err_t hub_portal_start(hub_state_t *st);
