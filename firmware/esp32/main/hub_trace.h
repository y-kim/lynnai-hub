#pragma once
#include <stdint.h>
#include "esp_http_server.h"

/*
 * hub_trace -- 기기(보일러·각방 Wi-Fi 모듈) ↔ 허브 ↔ 제조사 클라우드를 지나는 MQTT PUBLISH 를 순번·시각과 함께
 * 잠깐 쌓아 두고 GET /api/v1/trace 로 내준다. 제어기 버스 파형과 "무슨 메시지가 언제 오갔나"를 맞대 보려는 용도.
 *
 *   - 시각은 허브가 그 패킷을 본 순간의 부팅 후 ms(up_ms). 벽시계는 응답 머리의 up_ms/time_ms 쌍으로 환산한다.
 *   - PUBLISH 만 기록한다. CONNECT 에는 클라우드 계정·암호가 들어 있어 절대 기록하지 않는다.
 *   - RAM 8KB 고리 버퍼(메시지 수십 건). 넘치면 오래된 것부터 버리고, 응답의 lost 로 알려 준다.
 */

#define HUB_TR_DEV2CLOUD   0   /* 기기 → 클라우드 (허브가 중계하며 봄) */
#define HUB_TR_CLOUD2DEV   1   /* 클라우드 → 기기 (허브가 중계하며 봄) */
#define HUB_TR_HUB2DEV     2   /* 허브가 기기 쪽에 직접 써 넣음(클라우드가 보낸 것처럼) */

#define HUB_TR_SRC_DEVICE   0
#define HUB_TR_SRC_CLOUD    1  /* 제조사 클라우드(린나이 앱 조작 포함) */
#define HUB_TR_SRC_PLATFORM 2  /* SmartThings 명령(Zigbee/LAN 으로 받은 것) */
#define HUB_TR_SRC_POLL     3  /* 허브의 주기 상태 요청(30초) */
#define HUB_TR_SRC_TIME     4  /* 단독 모드: 허브의 시간 응답 */

void hub_trace_init(void);

/* MQTT 패킷 하나를 기록한다(PUBLISH 가 아니면 무시). pkt = 고정 헤더부터, hlen = 고정 헤더 길이.
 * 돌려주는 값은 순번(1부터, 기록하지 않았으면 0). */
uint32_t hub_trace_add(uint8_t dir, uint8_t src, uint8_t kind, const uint8_t mac3[3], const uint8_t *pkt, int n, int hlen);

uint64_t hub_trace_now_ms(void);                 /* 부팅 후 ms (기록과 같은 시계) */
esp_err_t hub_trace_http(httpd_req_t *req);      /* GET /api/v1/trace?since=<seq>&max=<n> — 인증은 호출 쪽 */
