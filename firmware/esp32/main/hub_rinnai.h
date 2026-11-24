#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * hub_rinnai -- 중계 중인 MQTT 를 엿봐 보일러(f06)·각방(f09) 상태를 뽑고, SmartThings 명령을 기기 쪽에 넣는다.
 *
 *   기기 → 클라우드 PUBLISH .../inf|res {"rpc":[{"id":"70","data":"<hex>"}]}  →  상태 블롭(바이트)  →  hub_uplink (Zigbee/LAN)
 *   플랫폼 명령 "f09 a0=7,b1=1"  →  기기 쪽에 클라우드가 보낸 것처럼 PUBLISH <기기 토픽>/set (J07)
 *
 * 명령은 링크 태스크가 클라우드→기기 MQTT 패킷 경계에서만 써 넣는다(중간에 끼면 스트림이 깨진다).
 */

#define HUB_KIND_F06 6      /* 보일러 */
#define HUB_KIND_F09 9      /* 각방 조절기 */
#define HUB_BLOB_MAX 64     /* EPC70 상태 블롭 최대 길이 */

/* 서버 API 용 기기 한 대 스냅샷(kind + MAC 끝3바이트로 식별) */
typedef struct {
    uint8_t kind;
    uint8_t mac3[3];
    uint8_t blob[HUB_BLOB_MAX];
    int n;
    bool linked;        /* 지금 중계 링크가 살아 있나 */
    uint32_t seq, chg_seq;   /* 상태를 실어 온 마지막 메시지 / 값이 바뀐 마지막 메시지의 기록 순번(/api/v1/trace) */
    uint64_t chg_ms;         /* 값이 마지막으로 바뀐 때(부팅 후 ms) */
    int16_t model;           /* 모델코드(토픽의 두 자리 16진수, 0x23 = "23"), 모르면 -1 — 온수·난방수 온도 표를 가른다 */
} hub_dev_t;

typedef struct hub_link_tap hub_link_tap_t;

/* 링크(기기 하나의 중계 연결)마다 하나. 링크 태스크에서만 부른다. */
hub_link_tap_t *hub_rinnai_link_open(uint32_t peer_ip);   /* peer_ip = 상대 주소(토픽 MAC 대조용, 0 = 대조 안 함) */
void hub_rinnai_link_close(hub_link_tap_t *t);
void hub_rinnai_tap_down(hub_link_tap_t *t, const uint8_t *data, int n);   /* 기기 → 클라우드 바이트 */
void hub_rinnai_tap_up(hub_link_tap_t *t, const uint8_t *data, int n);     /* 클라우드 → 기기 바이트 */
/* 기기 쪽에 써 넣을 명령 패킷이 있으면 out 에 담고 길이를 돌려준다(없거나 패킷 경계가 아니면 0). */
int hub_rinnai_take_cmd(hub_link_tap_t *t, uint8_t *out, int cap);

/* 플랫폼(Zigbee/LAN)에서 온 명령 텍스트: "f06@aabbcc b1=1" / "f09@ddeeff a0=7,d4=16" / "refresh"
 * ('@MAC끝3바이트' 로 대상 지정, 생략 시 그 종류의 아무 기기 = 단일 기기 하위호환), 여러 개는 ";" 로 */
void hub_rinnai_command(const char *text, int len);

void hub_rinnai_start(void);

/* 서버 API 용: 지금 아는 기기들을 out 에 채우고 개수를 돌려준다(최대 max) */
int hub_rinnai_snapshot(hub_dev_t *out, int max);

/* 관리 화면: 기기 삭제(MAC끝3 6hex). 찾아 지웠으면 true. */
bool hub_rinnai_forget(const char *mac6);

/* 명령 확인·재전송 누계와 온수 예열 자동 끄기 설정(API·관리 화면용) */
typedef struct {
    uint32_t tracked, resent, failed;   /* 켜진 뒤: 지켜본 명령 / 답이 없어 다시 보낸 횟수 / 3번 보내도 반영 안 된 명령 */
    uint8_t pending;                    /* 지금 확인을 기다리는 명령 수 */
    uint8_t preheat_off_min;            /* 온수 예열 자동 끄기(분, 0 = 무제한) */
    uint32_t preheat_on_s;              /* 예열이 켜진 지 몇 초(꺼져 있으면 0) */
    uint32_t preheat_left_s;            /* 자동으로 꺼지기까지 남은 초(꺼져 있으면 0, 무제한이면 UINT32_MAX) */
    uint8_t boil_mode;                  /* 보일러 모드 0 정하지 않음 / 1 난방수 / 2 온돌·실내 */
    char last_fail[48];
} hub_cmd_info_t;
void hub_rinnai_cmd_info(hub_cmd_info_t *out);
void hub_rinnai_render(char *o, size_t cap);                          /* 관리 화면 카드 "보일러 명령" */
bool hub_rinnai_apply(const char *form, char *msg, size_t msglen);   /* POST /admin/boiler (bmode=1 난방수 / 2 온돌·실내) */
esp_err_t hub_rinnai_set_boil_mode(uint8_t m);                      /* 1 난방수 / 2 온돌·실내(초기 설정·관리 화면) */
uint8_t hub_rinnai_boil_mode(void);
void hub_rinnai_request(const char *text, int len);                /* 스마트홈 쪽 요청 "preoff=<분>"(앞의 "hub " 는 뗀 것) */
/* 시험용(GET /api/v1/bootlog?cmddrop=<n> · preoff=<초>, 재부팅하면 풀림) */
void hub_rinnai_test_drop(int n);      /* 다음 명령 n 개를 기기로 보내지 않는다(보낸 것으로 치고 답을 기다린다) */
void hub_rinnai_test_preoff(int sec);  /* 온수 예열 자동 끄기를 초 단위로(0 = 설정대로) */
