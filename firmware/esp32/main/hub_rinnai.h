#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * hub_rinnai -- 중계 중인 MQTT 를 엿봐 보일러(f06)·각방(f09) 상태를 뽑고, SmartThings 명령을 기기 쪽에 넣는다.
 *
 *   기기 → 클라우드 PUBLISH .../inf|res {"rpc":[{"id":"70","data":"<hex>"}]}  →  상태 블롭(바이트)  →  Zigbee
 *   Zigbee 명령 "f09 a0=7,b1=1"  →  기기 쪽에 클라우드가 보낸 것처럼 PUBLISH <기기 토픽>/set (J07)
 *
 * 명령은 링크 태스크가 클라우드→기기 MQTT 패킷 경계에서만 써 넣는다(중간에 끼면 스트림이 깨진다).
 */

#define HUB_KIND_F06 6      /* 보일러 */
#define HUB_KIND_F09 9      /* 각방 조절기 */

typedef struct hub_link_tap hub_link_tap_t;

/* 링크(기기 하나의 중계 연결)마다 하나. 링크 태스크에서만 부른다. */
hub_link_tap_t *hub_rinnai_link_open(void);
void hub_rinnai_link_close(hub_link_tap_t *t);
void hub_rinnai_tap_down(hub_link_tap_t *t, const uint8_t *data, int n);   /* 기기 → 클라우드 바이트 */
void hub_rinnai_tap_up(hub_link_tap_t *t, const uint8_t *data, int n);     /* 클라우드 → 기기 바이트 */
/* 기기 쪽에 써 넣을 명령 패킷이 있으면 out 에 담고 길이를 돌려준다(없거나 패킷 경계가 아니면 0). */
int hub_rinnai_take_cmd(hub_link_tap_t *t, uint8_t *out, int cap);

/* Zigbee 에서 온 명령 텍스트: "f06 b1=1" / "f09 a0=7,b1=1,d4=16" / "refresh", 여러 개는 ";" 로 */
void hub_rinnai_command(const char *text, int len);

void hub_rinnai_start(void);

/* Zigbee 쪽(hub_zb_rinnai.c)이 구현: 가입돼 있을 때만 보낸다. */
void hub_zigbee_send_state(uint8_t kind, const uint8_t *blob, int n);
void hub_zigbee_send_links(uint8_t mask);
