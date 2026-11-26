#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "hub_rinnai.h"   /* hub_dev_t, HUB_BLOB_MAX */

/*
 * hub_upstream -- "멀티 허브": 여러 lynnai-hub 를 하나로 묶는다.
 *
 *   위성 허브(guest) ──TCP 8484(전화 거는 쪽)──▶ 중심 허브(host, 받는 쪽)
 *     중심(host): 위성들의 각방(f09)을 모아 자기 기기처럼 한 Zigbee 연결로 SmartThings 에 보여 준다.
 *                 위성이 올려보낸 STATE/LINKS/MODELS/LABELS/FORGET 의 f09 만 받고(보일러·가스 등은 버림),
 *                 ST 명령 f09@<위성mac> 은 그 위성 연결로 되돌려 보낸다. 위성 목록·삭제를 관리화면에서.
 *     위성(guest): 자기 각방을 중심으로 올려보낸다(= 기존 LAN 서버 로직을, 전화 거는 쪽으로). 보일러는 중심(한 대)만.
 *
 *   전선 위 프로토콜은 종전과 같다(종류별 [cmd][payload]). 바뀐 것은 "누가 전화를 거는가"(위성→중심)뿐.
 *   페어링: 중심에서 "다른 허브 연결 허용(3분)"(= hub_lan_pairing_open) 을 연 동안 위성이 접속, 위성 토큰을 중심이 저장.
 *
 * 허브 라벨(hub_label_*): 이 허브가 품은 방에 붙일 짧은 이름("1층"/"2층"). SmartThings/HA 방 이름 앞에 붙어 층을 구분.
 */

/* 모드 */
#define HUB_UP_OFF   0
#define HUB_UP_HOST  1    /* 중심 허브 */
#define HUB_UP_GUEST 2    /* 위성 허브 */

/* ---- 이 허브 라벨 (모든 허브 공통) ---- */
void hub_label_load(void);
int  hub_label_get(char *out, int cap);          /* 라벨 길이(0 = 없음). out 은 NUL 종단 */

/* ---- 멀티 허브 ---- */
void hub_upstream_start(void);                   /* 부팅 때 1회: 설정·라벨 읽기 */
bool hub_upstream_enabled(void);                 /* 모드가 꺼짐이 아님 */
int  hub_upstream_mode(void);                    /* HUB_UP_OFF/HOST/GUEST */
int  hub_upstream_guest_count(int *online);      /* 중심: 등록된 위성 수(온라인 수는 *online) */

/* 위성(guest) 다이얼러(hub_lan.c)용: 중심 허브 IP·이 허브 토큰 */
bool hub_upstream_guest_target(char *ip, int ipcap, char *tok, int tokcap);

/* 중심(host): hub_lan.c 인바운드가 "위성" 연결을 넘겨 준다 */
int  hub_upstream_guest_join(int fd, int tokslot, const char *id, const char *label, const char *ip); /* 위성 index, 꽉 차면 -1 */
void hub_upstream_guest_frame(int gi, uint8_t cmd, const uint8_t *p, int n);  /* 위성이 올린 프레임 처리 */
void hub_upstream_guest_leave(int gi, int fd);                                /* 위성 연결 끊김(이 fd 가 현재 연결일 때만 반영) */

/* push 루프(hub_rinnai)에서 로컬 목록 뒤에 원격 f09 를 덧붙인다 — hub_rinnai 잠금 **밖**에서 호출 */
int  hub_upstream_links(uint8_t *out, int cap);  /* ([kind][mac3]) * 온라인 원격 */
int  hub_upstream_models(uint8_t *out, int cap); /* ([kind][mac3][model]) * (모델 아는 것만) */
int  hub_upstream_labels(uint8_t *out, int cap); /* ([kind][mac3][len][라벨]) * (위성 라벨별) */
void hub_upstream_resend(void);                  /* beat 마다: 원격 STATE 다시 보냄(드라이버 재시작 동기화) */

bool hub_upstream_is_remote(uint8_t kind, const uint8_t *mac3);  /* 이 MAC 이 위성 기기인가 */
void hub_upstream_forward(const uint8_t *mac3, const char *text, int len);  /* mac3=NULL: 모든 위성에 방송(refresh). 아니면 그 위성으로 */

int  hub_upstream_snapshot(hub_dev_t *out, int max);            /* /api/v1/state 용 원격 기기 스냅샷 */
int  hub_upstream_label_of(uint8_t kind, const uint8_t *mac3, char *out, int cap);  /* 그 기기의 라벨(로컬=이 허브, 원격=위성). 0 = 없음 */

/* 관리 화면 */
void hub_upstream_render(char *o, size_t cap);
bool hub_upstream_apply(const char *form, char *msg, size_t len);   /* POST /admin/upstream */
