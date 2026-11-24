#pragma once
#include <stdint.h>
#include "esp_http_server.h"

/* 연결 품질 이벤트. NAS 기록기가 GET /api/events 로 가져가 빈도를 집계한다. */
typedef enum {
    EV_BOOT = 0,        /* a=esp_reset_reason */
    EV_AP_JOIN,         /* mac */
    EV_AP_LEAVE,        /* mac, a=사유(reason), b=붙어 있던 시간 ms */
    EV_STA_UP,          /* a=채널 */
    EV_STA_DOWN,        /* a=사유 */
    EV_LINK_OPEN,       /* a=기기 IP 끝자리 */
    EV_LINK_DEV_TLS,    /* a=IP 끝자리, b=소요 ms(음수=실패 코드) */
    EV_LINK_CLOUD_TLS,  /* a=IP 끝자리, b=소요 ms(음수=실패 코드) */
    EV_LINK_UP,         /* a=IP 끝자리 */
    EV_LINK_CLOSE,      /* a=IP 끝자리, b=유지 ms; mac[0]=닫은 쪽(1 기기 2 클라우드 3 유휴 0 미연결) */
    EV_LINK_REJECT,     /* a=IP 끝자리 */
    EV_BOOTSTRAP,       /* a=IP 끝자리, b=1 성공 0 실패 */
    EV_ZB_JOIN_FAIL,    /* a=상태 코드, b=다음 재시도 ms */
    EV_ZB_JOINED,       /* a=PAN, b=채널 */
    EV_ZB_LEAVE,        /* a=leave 타입 */
    EV_ONBOARD,         /* a=1 성공 0 실패 */
    EV_GAS_PULSE,       /* a=누적 펄스, b=1펄스 L */
    EV_STORE_BAD,       /* 저장 데이터 오류(hub_store). a=이번 부팅 누적 건수 — 자세한 내용은 /api/stats store_errors */
    EV_BOIL_BURN,       /* 보일러 연소 바뀜: a=비트(1 난방연소 2 온수연소), b=그 시점 가스 누적펄스. 가스↔연소 상관용 */
    EV_ROOM_OUT,        /* 각방 외출 바뀜: mac=f09 끝3, a=존 번호(0~6), b=1 외출 켬 / 0 끔 */
    EV_GAS_NO_PULSE,    /* 가스 계량기 '펄스 없음' 경고: a=1 시작(b=그동안 연소·난방한 초) / 0 풀림(펄스가 다시 옴) */
    EV_CMD_REVERT,      /* 명령이 상태에 반영됐다가 되돌아감(보일러가 안 받음): mac=기기 끝3, a=f0계열(6/9), b=반영~되돌림 초 */
    EV__COUNT
} hub_ev_t;

void hub_event(hub_ev_t type, const uint8_t *mac, int32_t a, int32_t b);

/* /api/events, /api/stats 를 포털 서버에 등록 */
void hub_events_register(httpd_handle_t srv);
uint32_t hub_events_boot_id(void);   /* 이번 부팅을 구분하는 임의 값(시각이 아님) */
void hub_events_print(int last);     /* USB 시리얼 진단용: 최근 이벤트를 printf 로 */
