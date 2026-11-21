#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_http_server.h"

/*
 * hub_dbgkey -- 원격 디버그 키(여러 개). OTA(펌웨어 올리기)·재부팅·진단을 집 밖/스크립트에서 쓰려는 용도.
 *
 * 서버 연동용 'API 토큰'(hub_api, 한 개·읽기 전용 성격)과 따로 둔다. 디버그 키는:
 *   - 여러 개를 별명과 함께 만들 수 있고,
 *   - 만들 때 비밀값을 '한 번만' 보여 준다(그 뒤엔 별명·만든 날짜만),
 *   - 고치는 건 없고 '삭제'만 된다.
 * 검증은 서버 토큰과 같은 모양: 요청 헤더 Authorization: Bearer <32자리 16진>.
 * 저장은 NVS("dbgkey"). 비밀값은 내부 플래시에만 있고 관리 화면엔 다시 안 나온다.
 */

void hub_dbgkey_render(char *out, size_t cap);          /* 관리 화면 '원격 디버그 키' 카드 */
/* 카드 안쪽만(AJAX 응답용). new_key 가 있으면 이번 한 번만 보여 준다. note = 결과 한 줄. */
void hub_dbgkey_render_body(char *out, size_t cap, const char *new_key, const char *note, bool ok);

/* 헤더의 Bearer 키가 저장된 디버그 키와 맞으면 true. 아무 응답도 쓰지 않는다(조용한 검사). */
bool hub_dbgkey_check(httpd_req_t *req);

/* 새 키 생성: 성공하면 1, out 에 32자리 비밀값(호출 쪽이 한 번만 보여 줌). 꽉 찼거나 저장 실패면 0. */
int  hub_dbgkey_create(const char *alias, char out[33]);
int  hub_dbgkey_delete(int idx);                        /* 1 성공 / 0 실패 */
