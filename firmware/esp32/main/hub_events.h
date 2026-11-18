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
    EV__COUNT
} hub_ev_t;

void hub_event(hub_ev_t type, const uint8_t *mac, int32_t a, int32_t b);

/* /api/events, /api/stats 를 포털 서버에 등록 */
void hub_events_register(httpd_handle_t srv);
