#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hub_state.h"

/* LED 오류 코드 판정(5초마다) + USB 시리얼 "diag" 명령. 자세한 건 hub_diag.c 머리말. */
void hub_diag_start(const hub_state_t *st);

/* Zigbee 상태를 WiFi(/api·관리화면)로 들여다보기 — 선 없이 진단. */
typedef struct {
    int ready, joined, factory_new, nvs_reset;
    uint16_t pan, short_addr;
    uint8_t channel;
    uint8_t eui[8];               /* 사람이 읽는 순서(상위 바이트부터) */
    int parent_rssi, parent_lqi;  /* 부모(코디네이터) 신호. parent_rssi==127 이면 모름 */
    int nbr_count;
} hub_zb_diag_t;
void hub_diag_zb_get(hub_zb_diag_t *out);
void hub_diag_zb_json(char *out, size_t cap);   /* {"ready":..,"joined":..,...} */
