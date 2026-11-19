#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * hub_plugins -- 허브에 꽂는 센서 '플러그인' 목록과 설정(켜짐·핀·값). 관리 화면에서 추가/삭제/핀 변경.
 * 설정은 NVS("plug") 에 저장되고, 저장 즉시 hub_sensors_reload() 로 센서만 다시 적용된다(재부팅 없음).
 *
 * 새 플러그인 추가 = hub_plugins.c 의 PLUGINS 표에 한 줄 + 그 플러그인 코드에서 hub_plugin_get() 으로 읽기.
 * 플랫폼(SmartThings)에는 켜진 플러그인 비트(HUB_RK_CMD_PLUGINS)를 보내, 꺼진 플러그인의 기기는 지운다.
 */

#define PLUG_ROOM  0x01     /* 온습도계 SHT31 */
#define PLUG_GAS   0x02     /* 가스 계량기 리드 스위치 */

/* 플러그인이 등록하는 '즉시 동작'(재부팅 없음): 값 문자열 하나를 받아 처리. msg 에 결과 문구를 쓴다. */
typedef bool (*hub_plugin_action_fn)(const char *value, char *msg, size_t msglen);
void hub_plugin_register_action(const char *id, const char *label, const char *hint, hub_plugin_action_fn fn);
bool hub_plugins_action(const char *form, char *msg, size_t msglen);   /* POST /admin/plugin-action */

void hub_plugins_load(void);
bool hub_plugin_enabled(const char *id);
int hub_plugin_get(const char *id, const char *key);
uint8_t hub_plugins_mask(void);                 /* 켜진 플러그인 비트 */

/* 관리 화면 */
void hub_plugins_render(char *out, size_t cap);  /* 카드들(HTML) */
/* 폼 적용: op = "save" | "add" | "del". 성공이면 true(호출부가 hub_sensors_reload), 실패면 err 에 이유 */
bool hub_plugins_apply(const char *form, char *err, size_t errlen);
