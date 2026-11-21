#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

/* 허브에 직접 꽂는 온습도계(SHT31) + 가스 계량기 펄스. 핀·형식은 hub_sensors.c 맨 위. */
void hub_sensors_start(void);
void hub_sensors_flush(void);    /* 일부러 재시작하기 직전: 가스 누적·사용량 칸을 플래시에 쓴다 */
void hub_sensors_reload(void);   /* 핀·설정 바뀜: 재부팅 없이 센서만 다시 적용 */
bool hub_sensors_set_reading(double m3);         /* 계량기 숫자 맞추기(관리 화면) */
/* 가스/온습도 값은 hub_api 가 스냅샷으로 가져가 /api/v1/state·/metrics 로 낸다(플러그인은 경로를 안 연다) */
void hub_sensors_refresh(void);
/* 고장으로 본 센서(PLUG_ROOM / PLUG_GAS 비트). 플랫폼에는 플러그인 알림의 둘째 바이트로 간다 → 그 기기 오프라인
 *   온습도계: 읽기가 3번 연속 실패(약 40초)   가스 계량기: 난방 중인 시간이 한도만큼 쌓이도록 펄스가 없음 */
uint8_t hub_sensors_faults(void);
void hub_sensors_print(void);                   /* USB diag */

/* 서버 API 용 */
typedef struct {
    uint32_t count, reading_l, hour_l, day_l, flow_lph;   /* reading_l = 0xFFFFFFFF 이면 계량기 숫자 미보정 */
} gas_view_t;
bool hub_sensors_room(float *t, float *h, int *age_s);  /* 마지막 온습도, 없으면 false */
bool hub_sensors_gas(gas_view_t *v, uint32_t *step_l);  /* 플러그인 꺼져 있으면 false */                   /* SmartThings refresh: 가스 값을 곧 다시 보낸다 */
