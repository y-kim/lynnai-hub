#pragma once
#include "hub_state.h"

/* LED 오류 코드 판정(5초마다) + USB 시리얼 "diag" 명령. 자세한 건 hub_diag.c 머리말. */
void hub_diag_start(const hub_state_t *st);
