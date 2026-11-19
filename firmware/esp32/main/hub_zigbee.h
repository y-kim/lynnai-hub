#pragma once
#include <stdbool.h>

/* Zigbee 망 찾기 허용(Wi-Fi 설정이 끝난 뒤). 그 전엔 설정용 AP 를 흔들지 않도록 찾지 않는다. */
void hub_zigbee_allow_steering(void);

/* 지금 바로 Zigbee 가입 시도 + 3분간 10초마다(SmartThings '기기 추가' 중에 쓴다).
 * 시도를 시작했으면 true(Zigbee 를 안 쓰거나, 이미 가입돼 있거나, Wi-Fi 설정 전이면 false). */
bool hub_zigbee_join_now(void);

/* 허브 AP 에 기기 등록(온보딩) 중인지 알린다. busy=true 면 2.4GHz 라디오를 Wi-Fi 에
 * 양보하려고 Zigbee 가입(steering)을 미룬다(그 반대는 가입 중 Wi-Fi 를 멈춤). */
void hub_zigbee_ap_onboarding(bool busy);
