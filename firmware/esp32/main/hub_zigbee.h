#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Zigbee 망 찾기 허용(Wi-Fi 설정이 끝난 뒤). 그 전엔 설정용 AP 를 흔들지 않도록 찾지 않는다. */
void hub_zigbee_allow_steering(void);

/* 마지막 Zigbee 가입 실패 기록(관리화면·/api 로 '왜 못 붙었는지' 표시).
 *  status      마지막 실패 상태코드(EZB_BDB_STATUS_*). count==0 이면 의미 없음
 *  count       가입 실패 누적 횟수
 *  age_s       마지막 실패가 몇 초 전인지(-1 = 실패 기록 없음)
 *  leave_type  마지막으로 망에서 '나감' 당한 종류(0xff = 없음) — SmartThings 에서 지워지면 들어온다
 *  leave_age_s 그 '나감'이 몇 초 전인지(-1 = 없음)
 * 안 쓰는 값은 NULL 가능. */
void hub_zigbee_last_fail(uint8_t *status, uint32_t *count, long *age_s,
                          uint8_t *leave_type, long *leave_age_s);
const char *hub_zigbee_fail_text(uint8_t status);   /* 상태코드 → 한국어 사유 */

/* 가입 정보가 없어 자동 가입 시도를 멈춘 상태인가(버튼·관리 화면으로만 다시 시작). */
bool hub_zigbee_auto_stopped(void);
/* 재가입 실패로 지워진 가입 데이터를 되살려 다시 시도하는 상태: 재시도 횟수, 한도, 재시작까지 남은 초(-1 = 예정 없음). */
void hub_zigbee_restore_state(int *tries, int *max, long *reboot_in_s);
int  hub_zigbee_stack_free(void);  /* Zigbee 태스크 스택이 가장 적게 남았던 바이트(0 = 태스크 없음) */
bool hub_zigbee_lost(void);       /* 가입했던 허브가 가입 정보를 잃은 상태인가(관리 화면 맨 위 경고용) */

/* 마지막 가입 정보 분실 기록(플래시에 남음). 기록이 없으면 false.
 *  kind  1 = 부팅 재가입 도중 탈퇴(분실), 2 = 가동 중 탈퇴(SmartThings 에서 삭제 추정)
 *  epoch 벽시계(0 = 모름), up_s 그 부팅에서 몇 초째였나. 안 쓰는 값은 NULL 가능. */
bool hub_zigbee_last_loss(int *kind, unsigned *count, long *epoch, long *up_s);

/* 지금 바로 Zigbee 가입 시도 + 3분간 10초마다(SmartThings '기기 추가' 중에 쓴다).
 * 시도를 시작했으면 true(Zigbee 를 안 쓰거나, 이미 가입돼 있거나, Wi-Fi 설정 전이면 false). */
bool hub_zigbee_join_now(void);
bool hub_zigbee_can_join(void);   /* join_now 가 시작할 조건인가(시작하지는 않음) */

/* 허브 AP 에 기기 등록(온보딩) 중인지 알린다. busy=true 면 2.4GHz 라디오를 Wi-Fi 에
 * 양보하려고 Zigbee 가입(steering)을 미룬다(그 반대는 가입 중 Wi-Fi 를 멈춤). */
void hub_zigbee_ap_onboarding(bool busy);
