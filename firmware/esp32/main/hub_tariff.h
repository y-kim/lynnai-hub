/*
 * hub_tariff -- 도시가스 요금 요소(보정계수·열량·단가·기본료) 중계 + 월별 요금 집계.
 *
 * SmartThings Edge 드라이버는 샌드박스 때문에 공개 인터넷에 못 나간다. 그래서 허브(집 Wi-Fi STA 로 인터넷
 * 연결)가 대신 gas.yangsu.kim/api/bill 을 받아 업링크 CMD 0x09 로 드라이버에 넘긴다(요금 계산은 드라이버).
 *
 * 모드:
 *   자동 — 구역 코드로 gas.yangsu.kim 에서 보정·열량·단가·기본료를 받아 중계(gas.yangsu.kim 은 펌웨어 고정).
 *   수동 — 관리 화면에 직접 넣은 값을 그대로 중계(인터넷 불필요).
 *
 * 월별: 허브가 쥔 가스 사용량(hub_sensors m³)과 유효 요금으로 검침일 기준 기간별 청구액을 계산해 최근 12개월을
 *       NVS 에 쌓는다(벽시계는 hub_api 의 SNTP·KST). /admin 그래프 + /api·/metrics 로 노출.
 *
 * 설정은 모두 NVS("gas"): region/tmode/tvcf/theat/trate/tbase/tbillday/tmon.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

void hub_tariff_init(void);                        /* 중계·집계 태스크 시작 */
void hub_tariff_render(char *out, size_t cap);     /* 관리 화면 '가스 요금' 카드(설정 + 월별 그래프) */

/* 관리 화면 저장(포털 POST). mode 0=자동,1=수동. 성공 true, 실패 시 err 에 사유. */
int  hub_tariff_apply(int mode, const char *region, int billday,
                      double vcf, double heat, double rate, double base,
                      char *err, int errlen);
void hub_tariff_refresh(void);     /* 지금 다시 받기(자동 모드 새로고침) */
void hub_tariff_now_html(char *out, size_t cap);   /* '현재값(자동 수신)' HTML 조각을 out 뒤에 덧붙임 */

/* API·metrics 노출용 — 현재 설정/유효 요금 */
void hub_tariff_cur(int *mode, const char **region, int *billday,
                    double *vcf, double *heat, double *rate, double *base, long *last_fetch);
/* 월별 기록: 개수와 i번째(0=가장 오래됨). m3·won 은 그 기간 사용량/청구액.
 * status: 0=진행 중, 1=기간 끝(잠정, 열량 확정 전), 2=확정. */
void hub_tariff_tick(void);        /* 월별 집계 최신화(API·metrics 진입 때 호출) */
int  hub_tariff_months(void);
int  hub_tariff_month_at(int i, int *year, int *month, double *m3, uint32_t *won, int *closed);
