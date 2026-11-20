#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * hub_devs -- 등록된 린나이 기기 목록(MAC 6바이트).
 *
 * 허브 AP 에 붙었다고 곧바로 기기로 받지 않는다. 등록된 MAC 만 상태를 읽고 SmartThings 에 내보내며 명령을 받는다.
 * 등록되는 길은 셋:
 *   - 허브 버튼(길게)으로 한 기기 등록이 성공하면 그 뒤 HUB_DEVS_BUTTON_S 초 동안 새로 붙는 기기는 자동 등록
 *   - 관리 화면 "등록 대기"에서 등록 버튼(허브 AP 에 붙어 있는데 등록 안 된 단말이 거기 보인다)
 *   - 이 기능이 없던 펌웨어에서 올라온 첫 부팅: HUB_DEVS_FIRST_S 초 동안 붙는 기기는 그대로 등록(쓰던 기기 옮기기)
 * 등록 안 된 단말의 연결은 끊지 않는다(프록시 모드면 클라우드로 그대로 이어 줘 린나이 앱은 된다). 허브가 그 상태를
 * 읽거나 SmartThings 에 보이지 않을 뿐이다.
 */
#define HUB_DEVS_MAX       8
#define HUB_DEVS_BUTTON_S  180
#define HUB_DEVS_FIRST_S   600

void hub_devs_load(void);
bool hub_devs_allowed(const uint8_t mac6[6]);          /* 등록됐나. 자동 등록 창이 열려 있으면 등록하고 true */
esp_err_t hub_devs_add(const uint8_t mac6[6]);
bool hub_devs_remove3(const uint8_t mac3[3]);          /* MAC 끝 3바이트가 같은 항목을 지운다(기기 삭제) */
void hub_devs_auto_open(int seconds);                  /* 지금부터 seconds 초 동안 새로 붙는 기기는 자동 등록 */

void hub_devs_render(char *out, size_t cap);                      /* 관리 화면 "등록 대기" */
bool hub_devs_apply(const char *form, char *msg, size_t msglen);  /* POST /admin/dev-approve (approve=<MAC 12자리>) */
void hub_devs_print(void);                                        /* USB diag: 등록된 기기 + 등록 대기 단말 */
void hub_devs_usb(const char *arg);   /* USB "devs [add <MAC 12자리> | unreg <MAC 끝 6자리>]" — 물리 접근 = 관리자 권한 */
