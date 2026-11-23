#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_http_server.h"

/*
 * hub_bbox -- 부팅 기록(블랙박스). USB 없이 "부팅 직후 Zigbee 재가입이 왜 실패했나"를 보려는 것.
 *
 * 재가입 실패는 Wi-Fi 가 켜지기 전에 일어나고, 곧바로 허브가 재시작하므로(가입 정보 되살리기) 그 순간을 볼 길이 없었다.
 *   1. 로그: 이번 부팅과 직전 부팅의 로그를 재시작해도 지워지지 않는 RAM(.noinit)에 담는다. 부팅마다 앞부분(HEAD)과
 *      마지막 부분(TAIL)을 남긴다. 전원이 끊기거나 다른 빌드로 OTA 하면 직전 부팅 기록은 없다(같은 빌드의 재시작만).
 *      Zigbee 스택이 내는 디버그 수준 메시지도 같이 담는다(기본 플랫폼 로거는 INFO 까지만 내보낸다).
 *   2. 무선 카운터: 802.15.4 하드웨어가 세는 값(ACK 못 받음·채널 혼잡·CRC 오류 등)을 재가입 결과마다 한 줄로 남긴다.
 *   3. 프레임 카운터: 재시작 직전 값과 다음 부팅에서 스택이 들고 올라온 값을 비교한다.
 *   4. 부팅 이력: 부팅마다 재시작 사유·재가입 결과·걸린 시간·실패 횟수를 플래시(NVS)에 48건 남긴다.
 *
 * GET /api/v1/bootlog 로 읽는다(part=hist|prev|cur|tasks, 기본은 hist·prev·cur. tasks = 힙 요약과 태스크별 스택 최소 여유. zblvl=<0~5> 로 스택 로그 수준 바꿈).
 *   cloudblock=<0|1|2> 시험(자동 폴백 확인용, 재부팅하면 풀림): 1 = 린나이 서버에 닿지 않는 것처럼, 2 = 서버가 로그인을 거절하는 것처럼
 *   reboot=<분> 주기 재부팅(0 = 끔, 5~1440, 저장됨) — 재가입 실패를 다시 잡으려는 시험. 붙어 있고 이번 부팅에 실패가
 *   없을 때만 재시작하고, 실패가 잡힌 부팅에서는 멈춘다(기록 보존). 멈춘 부팅에서 다시 주면 이어서 한다.
 */

void hub_bbox_init(void);            /* app_main 맨 앞: 로그 담기 시작(NVS 없이도 됨) */
void hub_bbox_start(void);           /* NVS 를 연 뒤: 부팅 이력에 이번 부팅을 올리고 프레임 카운터 감시 시작 */

void hub_bbox_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   /* 기록에만 남기는 한 줄(콘솔 X) */
void hub_bbox_radio(const char *when);        /* 무선 카운터가 지난번 이후 얼마나 늘었나 한 줄 */

void hub_bbox_zb_boot(uint32_t fc);           /* 스택이 뜸: 저장돼 있던 프레임 카운터 */
void hub_bbox_zb_fail(uint8_t status);        /* 가입/재가입 실패 */
void hub_bbox_zb_joined(void);                /* 망에 붙음 */
void hub_bbox_zb_left(uint8_t leave_type, bool while_up);   /* 망에서 나감(가입 정보 지워짐) */

void hub_bbox_before_restart(void);  /* 일부러 재시작하기 직전: 마지막 프레임 카운터를 플래시에도 적어 둔다 */

esp_err_t hub_bbox_http(httpd_req_t *req);    /* GET /api/v1/bootlog — 인증은 호출 쪽 */

/* GET /api/v1/zbscan — 스캔 시험(비콘 수신률). n=<횟수> 를 주면 시작하고, 없으면 마지막 결과만 돌려준다.
 *   dur=<0~8> 스캔 시간 지수((2^dur+1)*15.36ms, 기본 3) · alt=1 홀수 번째는 dur+2 · gap=<ms> · pause=1 Wi-Fi 를 멈추고
 *   ed=1 채널 소음(에너지) 스캔도(붙어 있는 동안에는 쓰지 말 것 — 스택이 끝내지 못하고 스캔이 막힌다)
 *   setdur=<0~8|255> 스택이 가입·재가입에 쓰는 스캔 시간(255 = 기본값, 저장됨) · onfail=<0|1> 가입 실패 직후 자동 스캔
 *   minlqi=<0~254|255> 스택이 부모로 받아 주는 비콘의 최소 신호 품질(허브 기본 40, 255 = 스택 기본값 75, 저장됨, 바로 적용) */
esp_err_t hub_bbox_scan_http(httpd_req_t *req);   /* auto=<초> 붙어 있는 동안 주기 측정(0 = 끔, 저장됨) */
void hub_bbox_scan_stats(uint32_t *total, uint32_t *hit, int *rssi, int *ed);   /* 주기 측정 누계(/metrics 용) */
