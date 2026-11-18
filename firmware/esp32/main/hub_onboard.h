#pragma once
#include "hub_state.h"

/* 보일러 온보딩에 쓸 허브 상태(허브 AP SSID/암호)를 등록. app_main 에서 한 번. */
void hub_onboard_init(const hub_state_t *st);

/* 버튼 짧게 누름 처리: 보일러 SoftAP 를 찾아 허브 AP 정보를 넘긴다(한 번에 하나만 실행). */
void hub_onboard_trigger(void);
