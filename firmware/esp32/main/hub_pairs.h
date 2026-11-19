#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_http_server.h"

/*
 * hub_pairs -- 보일러(f06)↔써머스탯(f09) 짝을 관리 화면에서 수동 지정하고 저장한다(NVS "pairs").
 * MQTT 토픽엔 짝 정보가 없어서, 어느 f09 가 어느 f06 에 붙는지는 사용자가 정한다.
 * 짝은 HUB_RK_CMD_PAIRS 로 드라이버에 전해져 연동(방↔보일러)·단독 판정에 쓰인다.
 * 식별자는 MAC 끝 3바이트(6 hex).
 */
void hub_pairs_load(void);
int hub_pairs_blob(uint8_t *out, int cap);          /* ([f09 mac3][f06 mac3])*n — 드라이버로 보낼 것 */

/* 관리 화면 */
void hub_pairs_render(char *out, size_t cap);        /* "연결된 기기"(별명·보일러 연결) 표 */
bool hub_pairs_apply(const char *form, char *msg, size_t msglen);   /* POST /admin/devices (별명+짝 저장) */
bool hub_pairs_delete(const char *form, char *msg, size_t msglen);  /* POST /admin/device-del (기기 삭제) */
