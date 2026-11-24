/*
 * hub_rinnai.c -- 중계 MQTT 엿보기(상태) + 명령 주입. 설명은 hub_rinnai.h.
 *
 * 기기를 종류(f06/f09)가 아니라 MAC 끝 3바이트(instance)로 구분한다 → 보일러 N대·써머스탯 N개.
 * 플랫폼으로 보내는 상태/명령에도 MAC 끝3바이트가 들어간다(hub_uplink.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "hub_rinnai.h"
#include "hub_events.h"
#include "hub_sensors.h"
#include "hub_wifi.h"
#include "hub_proxy.h"
#include "hub_uplink.h"
#include "hub_pairs.h"
#include "hub_expose.h"
#include "hub_devs.h"
#include "hub_trace.h"
#include "hub_main.h"
#include "hub_store.h"
#include "esp_http_server.h"

static const char *TAG = "HUB_RINNAI";

#define CAP_DOWN    768             /* 기기→클라우드 패킷 하나 (f09 res ~300바이트) */
#define CMD_MAX     320
#define CAP_UP      512             /* 클라우드→기기 패킷 하나(명령·상태 요청은 작다). 넘는 패킷은 기록만 건너뜀 */
#define HEARTBEAT_S 300             /* 바뀐 게 없어도 이 간격으로 다시 보낸다(드라이버가 끊김 판단) */
#define ALIVE_S     20              /* 생존 신호 간격(아래 루프가 10초마다 깨므로 실제 20~30초). 드라이버는 90초 침묵이면 끊김으로 본다 */
#define POLL_S      30              /* 이 간격으로 보일러/각방에 상태 요청(fa) → 외출 등 변경 빨리 반영 */
#define MAX_DEV     6               /* 동시에 다룰 기기 수(보일러 + 써머스탯 합) */
#define MAX_PAIRS_BYTES 36          /* 짝 6개 * 6바이트 */

/* MQTT 패킷 경계 추적. cap>0 이면 패킷 전체를 모아 cb 로 넘긴다. */
typedef struct {
    uint8_t hdr[5];
    int hlen;
    uint32_t left;
    bool in_body;
    uint8_t *pkt;
    int cap, plen;
    bool over;
} framer_t;

typedef struct {
    int len;
    uint8_t src;                    /* 누가 만든 패킷인가(HUB_TR_SRC_*) — 메시지 기록용 */
    int8_t pend;                    /* 확인을 기다리는 명령이면 그 칸(s_pend), 아니면 -1 */
    uint16_t pend_gen;
    uint8_t pkt[CMD_MAX];
} cmd_t;

struct hub_link_tap {
    framer_t down, up;
    uint8_t kind;                   /* 0 = 아직 모름 */
    uint8_t mac3[3];                /* MAC 끝 3바이트 = instance */
    char base[72];                  /* rinnai/RK/01/RK/f06/23/<MAC> */
    QueueHandle_t cmds;
    uint32_t peer_ip;               /* 이 연결의 상대 주소(허브 AP 대역) */
    bool refused;                   /* 토픽 MAC 이 상대와 안 맞아 거절함(로그 한 번만) */
    bool pending;                   /* 등록 안 된 기기라 기기로 보지 않는 중(로그 한 번만) */
    bool used;
    bool poll_now;                  /* 단독 모드: 기기가 /get 을 구독하면 곧바로 상태 요청(fa) */
};

#define MAX_TAPS 3
static hub_link_tap_t s_taps[MAX_TAPS];
static SemaphoreHandle_t s_lock;

/* 기기별 마지막 상태(kind+mac3 으로 식별) */
typedef struct {
    bool used;
    uint8_t kind;
    uint8_t mac3[3];
    uint8_t blob[HUB_BLOB_MAX];
    int n;
    bool dirty;
    uint32_t seq, chg_seq;   /* 상태(70)를 실어 온 마지막 메시지 / 값이 바뀐 마지막 메시지의 기록 순번(hub_trace) */
    uint64_t chg_ms;         /* 값이 마지막으로 바뀐 때(부팅 후 ms) */
    uint8_t burn;       /* f06: 마지막 연소 상태(비트 1 난방 2 온수) — 바뀔 때만 이벤트 */
    uint64_t pre_on_ms; /* f06: 온수 예열이 켜진 것을 처음 본 때(부팅 후 ms, 0 = 꺼져 있음) — 자동 끄기용 */
    uint64_t pre_try_ms;/* f06: 자동 끄기 명령을 마지막으로 보낸 때 */
    uint64_t pre_off_at;/* f06: 이때 끈다(부팅 후 ms, 0 = 무제한). 켜질 때 기본값으로 정하고, 켜진 동안 고르면 그 값으로 다시 정한다 */
    uint8_t out_mask;   /* f09: 마지막 존별 외출 비트 — 바뀐 존만 이벤트 */
} rk_dev_t;

/* f06 연소·f09 외출 추출용. 각방 stat/현재온도 바이트 위치(hub_api.c ZONES 와 같은 순서 z1,z2,z4,z5,z6,z7,z3) */
#define RK_NZ 7
static const uint8_t RK_ZSTAT[RK_NZ] = { 0, 7, 14, 21, 28, 35, 42 };
static const uint8_t RK_ZCUR[RK_NZ]  = { 3, 10, 17, 24, 31, 38, 45 };
static int bnib(const uint8_t *b, int i) { return i % 2 ? b[i / 2] & 0xf : b[i / 2] >> 4; }
static rk_dev_t s_dev[MAX_DEV];

/* 기기 모델코드(토픽 …/f06/<두 자리>/<MAC> 의 두 자리 16진수) — 앱은 이것으로 온수·난방수 온도 표를 고른다.
 * 연결이 끊겨도 기억한다(재부팅하면 다음 연결 때 다시 배운다). */
typedef struct {
    bool used;
    uint8_t kind;
    uint8_t mac3[3];
    uint8_t model;
} rk_model_t;
static rk_model_t s_model[MAX_DEV];

static void remember_model(uint8_t kind, const uint8_t mac3[3], const char *base)
{
    const char *p = strstr(base, kind == HUB_KIND_F06 ? "/f06/" : "/f09/");
    unsigned v;
    if (!p || sscanf(p + 5, "%2x", &v) != 1 || !isxdigit((unsigned char)p[5]) || !isxdigit((unsigned char)p[6]) || p[7] != '/') {
        return;
    }
    rk_model_t *free_slot = NULL;
    for (int i = 0; i < MAX_DEV; i++) {
        if (s_model[i].used && s_model[i].kind == kind && !memcmp(s_model[i].mac3, mac3, 3)) {
            s_model[i].model = v;
            return;
        }
        if (!s_model[i].used && !free_slot) {
            free_slot = &s_model[i];
        }
    }
    if (free_slot) {
        *free_slot = (rk_model_t){ .used = true, .kind = kind, .model = v };
        memcpy(free_slot->mac3, mac3, 3);
    }
}

static int model_of(uint8_t kind, const uint8_t mac3[3])   /* 잠금 안에서. 모르면 -1 */
{
    for (int i = 0; i < MAX_DEV; i++) {
        if (s_model[i].used && s_model[i].kind == kind && !memcmp(s_model[i].mac3, mac3, 3)) {
            return s_model[i].model;
        }
    }
    return -1;
}

static bool s_links_dirty = true;   /* 연결 목록을 다시 보낼지 */
static bool s_expose_dirty;         /* 노출 마스크를 다시 보낼지(refresh 가 표시, zb_task 가 보냄) */
static TaskHandle_t s_task;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void kick(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static bool mac3_eq(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 3); }

/* ---------------------------------------------------------------- 명령 확인·재전송
 * 명령(/set)은 QoS 0 으로 한 번 써 넣을 뿐이라 가끔 그냥 사라진다(2026-11-23 실측: 약 90번에 2번 — 제어기가 자기 보고를
 * 올린 직후에 들어간 명령). 그래서 보낸 명령이 바꾸려던 칸을 그 뒤의 상태 보고(70)에서 확인하고, PEND_WAIT_MS 안에
 * 나타나지 않으면 같은 명령을 다시 보낸다(모두 PEND_TRIES 번, 린나이 앱과 같은 2.5초·3번).
 *   - "아무 보고나 오면 성공"으로 보지 않는다: 보일러는 온도 값이 흔들릴 때마다 4~12초 간격으로 보고를 올린다.
 *     바꾸려던 칸을 모르는 명령만 "보낸 뒤 온 첫 보고"를 답으로 친다.
 *   - 칸이 바뀌었다 = 제어기가 명령을 받았다. 보일러가 그 값을 안 받으면 제어기는 20초쯤 뒤 원래 값으로 되돌리는데
 *     (꺼진 채 온돌 설정 등), 그건 다시 보내도 같으므로 여기서 다루지 않는다.
 *   - 같은 칸에 새 명령이 오면 옛 명령은 잊는다(켰다 바로 끈 경우 옛 '켜기'를 다시 보내지 않게). */
#define PEND_MAX      8
#define PEND_WAIT_MS  2500
#define PEND_TRIES    3
#define PEND_QUEUE_MS 10000         /* 주입 큐에서 이만큼 못 나가면(연결이 막힘) 포기 */
#define PEND_ANY_MS   300           /* 칸을 모르는 명령: 보낸 지 이만큼 지난 뒤의 보고만 답으로 친다 */
#define CHK_MAX       3
enum { CHK_NIB, CHK_HEX2, CHK_BIT, CHK_BYTE };
typedef struct {
    uint8_t type;                   /* CHK_NIB 니블이 0 이 아님 == want · CHK_HEX2 니블 둘의 값 == want (f06, pos = 니블 위치) */
    uint8_t pos, mask, want;        /* CHK_BIT (바이트 & mask) == want · CHK_BYTE 바이트 == want (f09, pos = 바이트 위치) */
} chk_t;
typedef struct {
    bool used;
    uint8_t kind, mac3[3];
    uint8_t tries;                  /* 지금까지 보낸 횟수 */
    uint8_t src;
    uint8_t nchk;
    chk_t chk[CHK_MAX];
    uint16_t gen;
    uint16_t jlen;
    uint64_t made_ms, sent_ms;      /* 큐에 넣은 때 / 기기 쪽 소켓에 써 넣은 때(0 = 아직 큐에 있음) */
    char key[24];                   /* 바꾸려는 칸 이름(코드들, 각방은 a0 값까지) — 같은 칸의 새 명령이 옛 명령을 밀어낸다 */
    char json[256];
} pend_t;
static pend_t s_pend[PEND_MAX];
static uint16_t s_pend_gen;
static uint32_t s_cmd_tracked, s_cmd_resent, s_cmd_failed;
static char s_cmd_last_fail[48];    /* 끝내 반영되지 않은 마지막 명령(관리 화면용) */
static uint8_t s_pre_off_min;       /* 온수 예열 자동 끄기(분, 0 = 끄지 않음) */
static uint8_t s_boil_mode;         /* 보일러 모드: 0 정하지 않음(드라이버가 조절기 연결로 짐작), 1 난방수, 2 온돌·실내 */
static volatile uint8_t s_test_drop; /* 시험: 다음 명령 n 개를 기기로 안 보냄 */
static volatile uint32_t s_test_preoff_s;   /* 시험: 자동 끄기 시간(초, 0 = 설정대로) */
static volatile bool s_boilcfg_dirty;       /* 예열이 켜지거나 꺼짐 → 남은 시간을 바로 알린다 */

static bool chk_ok(const chk_t *c, const uint8_t *b, int bn)
{
    if (c->type == CHK_NIB || c->type == CHK_HEX2) {
        int last = c->pos + (c->type == CHK_HEX2 ? 1 : 0);
        if (last / 2 >= bn) {
            return false;
        }
        if (c->type == CHK_NIB) {
            return (bnib(b, c->pos) != 0) == (c->want != 0);
        }
        return ((bnib(b, c->pos) << 4) | bnib(b, c->pos + 1)) == c->want;
    }
    if (c->pos >= bn) {
        return false;
    }
    return c->type == CHK_BIT ? (b[c->pos] & c->mask) == c->want : b[c->pos] == c->want;
}

static bool chk_all(const chk_t *c, int n, const uint8_t *b, int bn)
{
    for (int i = 0; i < n; i++) {
        if (!chk_ok(&c[i], b, bn)) {
            return false;
        }
    }
    return true;
}

/* 명령 항목(code=value 들)에서 확인할 칸을 뽑는다. 돌려주는 값 = 칸 수(0 = 모르는 명령). key 에 칸 이름.
 *   f06: 80 전원(니블 0) b1 난방(2) d7 온수(3) b6 예열(5) b4 실내 모드(7) d1 온수 설정(10~11) d2 온돌 설정(13~14) d4 실내 설정(15~16)
 *   f09: a0 = 방 번호(1~7, stat 바이트 = (a0-1)*7) + b1 켜짐(0x02) b3 외출(0x10) 96 예약(0x08) b4 시간 모드(0x04) 97 예약 시간(stat+1) d4 설정 온도(stat+4) */
static int build_checks(uint8_t kind, char codes[][3], const uint32_t *vals, int n, chk_t *out, char *key, size_t keycap)
{
    int nc = 0, zone = -1;
    key[0] = 0;
    for (int i = 0; i < n; i++) {
        if (kind == HUB_KIND_F09 && !strcmp(codes[i], "a0") && vals[i] >= 1 && vals[i] <= RK_NZ) {
            zone = (int)vals[i] - 1;
        }
        char one[8];
        if (!strcmp(codes[i], "97")) {               /* 예약 시간은 칸 이름에 넣지 않는다(예약 걸기와 풀기가 같은 칸이 되게) */
            continue;
        }
        if (!strcmp(codes[i], "a0")) {
            snprintf(one, sizeof(one), "a0=%x,", (unsigned)(vals[i] & 0xff));
        } else {
            snprintf(one, sizeof(one), "%s,", codes[i]);
        }
        strlcat(key, one, keycap);
    }
    for (int i = 0; i < n && nc < CHK_MAX; i++) {
        const char *c = codes[i];
        uint8_t v = (uint8_t)vals[i];
        if (kind == HUB_KIND_F06) {
            static const struct { const char *code; uint8_t type, pos; } F06[] = {
                { "80", CHK_NIB, 0 }, { "b1", CHK_NIB, 2 }, { "d7", CHK_NIB, 3 }, { "b6", CHK_NIB, 5 }, { "b4", CHK_NIB, 7 },
                { "d1", CHK_HEX2, 10 }, { "d2", CHK_HEX2, 13 }, { "d4", CHK_HEX2, 15 } };
            for (size_t k = 0; k < sizeof(F06) / sizeof(F06[0]); k++) {
                if (!strcmp(c, F06[k].code)) {
                    out[nc++] = (chk_t){ .type = F06[k].type, .pos = F06[k].pos, .want = v };
                    break;
                }
            }
        } else if (zone >= 0) {
            uint8_t st = (uint8_t)(zone * 7);
            uint8_t bit = !strcmp(c, "b1") ? 0x02 : !strcmp(c, "b3") ? 0x10 : !strcmp(c, "96") ? 0x08 : !strcmp(c, "b4") ? 0x04 : 0;
            if (bit) {
                out[nc++] = (chk_t){ .type = CHK_BIT, .pos = st, .mask = bit, .want = v ? bit : 0 };
            } else if (!strcmp(c, "97")) {
                out[nc++] = (chk_t){ .type = CHK_BYTE, .pos = (uint8_t)(st + 1), .want = v };
            } else if (!strcmp(c, "d4")) {
                out[nc++] = (chk_t){ .type = CHK_BYTE, .pos = (uint8_t)(st + 4), .want = v };
            } else if (!strcmp(c, "da")) {               /* 시간 난방: 1시간에 몇 분 */
                out[nc++] = (chk_t){ .type = CHK_BYTE, .pos = (uint8_t)(st + 5), .want = v };
            }
        }
    }
    return nc;
}

static void pend_done(pend_t *q, const char *how)
{
    if (q->tries > 1) {
        ESP_LOGW(TAG, "명령 %s — f0%d %02x%02x%02x %s(%d번 보냄)", how, q->kind, q->mac3[0], q->mac3[1], q->mac3[2], q->key, q->tries);
    }
    q->used = false;
}

/* 기기가 상태(70)를 올렸다: 기다리던 명령이 반영됐는지 본다. 호출자가 lock 보유. */
static void pend_report(uint8_t kind, const uint8_t *mac3, const uint8_t *blob, int bn)
{
    uint64_t now = hub_trace_now_ms();
    for (int i = 0; i < PEND_MAX; i++) {
        pend_t *q = &s_pend[i];
        if (!q->used || q->kind != kind || !mac3_eq(q->mac3, mac3)) {
            continue;
        }
        if (q->nchk ? chk_all(q->chk, q->nchk, blob, bn) : (q->sent_ms && now - q->sent_ms >= PEND_ANY_MS)) {
            pend_done(q, "반영됨");
        }
    }
}

/* base 토픽의 마지막 세그먼트(MAC "AA:BB:CC:A1:B2:C3")에서 끝 3바이트를 뽑는다 */
static bool parse_mac3(const char *base, uint8_t out[3])
{
    const char *m = strrchr(base, '/');
    m = m ? m + 1 : base;
    uint8_t b[6];
    int n = 0;
    for (const char *p = m; *p && n < 6; ) {
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
            return false;
        }
        char hx[3] = { p[0], p[1], 0 };
        b[n++] = (uint8_t)strtoul(hx, NULL, 16);
        p += 2;
        if (*p == ':') {
            p++;
        }
    }
    if (n < 3) {
        return false;
    }
    memcpy(out, b + n - 3, 3);       /* 끝 3바이트 */
    return true;
}

static rk_dev_t *dev_find(uint8_t kind, const uint8_t *mac3)
{
    for (int i = 0; i < MAX_DEV; i++) {
        if (s_dev[i].used && s_dev[i].kind == kind && mac3_eq(s_dev[i].mac3, mac3)) {
            return &s_dev[i];
        }
    }
    return NULL;
}

static rk_dev_t *dev_get(uint8_t kind, const uint8_t *mac3)
{
    rk_dev_t *d = dev_find(kind, mac3);
    if (d) {
        return d;
    }
    for (int i = 0; i < MAX_DEV; i++) {
        if (!s_dev[i].used) {
            s_dev[i].used = true;
            s_dev[i].kind = kind;
            memcpy(s_dev[i].mac3, mac3, 3);
            s_dev[i].n = 0;
            s_dev[i].dirty = false;
            s_dev[i].burn = 0;
            s_dev[i].out_mask = 0;
            return &s_dev[i];
        }
    }
    return NULL;                     /* 표 가득 */
}

/* ---------------------------------------------------------------- MQTT 틀 */

typedef void (*pkt_cb_t)(hub_link_tap_t *t, const uint8_t *p, int n, int hlen);

static void framer_feed(hub_link_tap_t *t, framer_t *f, const uint8_t *d, int n, pkt_cb_t cb)
{
    int i = 0;
    while (i < n) {
        if (!f->in_body) {
            uint8_t b = d[i++];
            f->hdr[f->hlen++] = b;
            if (f->hlen >= 2 && !(b & 0x80)) {
                uint32_t rl = 0, mul = 1;
                for (int k = 1; k < f->hlen; k++, mul *= 128) {
                    rl += (f->hdr[k] & 0x7f) * mul;
                }
                f->left = rl;
                f->in_body = true;
                f->plen = 0;
                f->over = f->cap < f->hlen + (int)rl;
                if (!f->over) {
                    memcpy(f->pkt, f->hdr, f->hlen);
                    f->plen = f->hlen;
                }
            } else if (f->hlen == 5) {               /* 길이 필드가 4바이트를 넘음: 틀이 깨짐, 처음부터 */
                f->hlen = 0;
                continue;
            }
            if (!f->in_body) {
                continue;
            }
        } else {
            uint32_t take = (uint32_t)(n - i) < f->left ? (uint32_t)(n - i) : f->left;
            if (!f->over) {
                memcpy(f->pkt + f->plen, d + i, take);
                f->plen += take;
            }
            f->left -= take;
            i += take;
        }
        if (f->in_body && f->left == 0) {
            if (cb && !f->over) {
                cb(t, f->pkt, f->plen, f->hlen);
            }
            f->in_body = false;
            f->hlen = 0;
        }
    }
}

static bool framer_at_boundary(const framer_t *f) { return !f->in_body && f->hlen == 0; }

/* 기기가 보낸 PUBLISH 에서 토픽·상태 블롭을 뽑는다 */
static bool enqueue_pub(hub_link_tap_t *t, const char *topic, int tl, const char *json, int j, uint8_t src);

/* base 토픽 끝의 MAC 6바이트 */
static bool parse_mac6(const char *base, int len, uint8_t out[6])
{
    char tmp[80];
    if (len <= 0 || len >= (int)sizeof(tmp)) {
        return false;
    }
    memcpy(tmp, base, len);
    tmp[len] = 0;
    const char *m = strrchr(tmp, '/');
    m = m ? m + 1 : tmp;
    int n = 0;
    for (const char *p = m; *p && n < 6;) {
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1])) {
            return false;
        }
        char hx[3] = { p[0], p[1], 0 };
        out[n++] = (uint8_t)strtoul(hx, NULL, 16);
        p += 2;
        if (*p == ':') {
            p++;
        }
    }
    return n == 6;
}

/* 이 링크의 기기 토픽(base = 마지막 '/' 앞)과 종류를 배운다 */
static void learn_base(hub_link_tap_t *t, const char *topic, const char *slash)
{
    uint8_t kind = strstr(topic, "/f06/") ? HUB_KIND_F06 : strstr(topic, "/f09/") ? HUB_KIND_F09 : 0;
    if (!kind || slash - topic >= (int)sizeof(t->base) || t->kind == kind || t->refused) {
        return;
    }
    /* 토픽의 MAC 이 실제로 이 연결의 상대(허브 AP 에 붙은 그 단말)인지 — 남의 기기 행세 막기 */
    uint8_t mac6[6];
    if (!parse_mac6(topic, slash - topic, mac6) || (t->peer_ip && !hub_wifi_ap_peer_is(t->peer_ip, mac6))) {
        t->refused = true;
        ESP_LOGW(TAG, "토픽의 기기 MAC 이 연결한 단말과 다름 — 이 연결은 기기로 보지 않음: %.*s", (int)(slash - topic), topic);
        return;
    }
    /* 등록된 기기만 기기로 본다(kind 를 배우지 않으면 상태 읽기·연결 목록·명령이 모두 이 연결을 건너뛴다).
     * 등록 안 된 단말의 중계 자체는 그대로 둔다. 관리 화면 "등록 대기"에서 등록하면 다음 연결부터 알아본다. */
    if (!hub_devs_allowed(mac6)) {
        if (!t->pending) {
            t->pending = true;
            ESP_LOGW(TAG, "등록 안 된 기기 %02x:%02x:%02x:%02x:%02x:%02x (f0%d) — 관리 화면 '등록 대기'에서 등록해야 읽는다", mac6[0],
                     mac6[1], mac6[2], mac6[3], mac6[4], mac6[5], kind);
        }
        return;
    }
    lock();
    t->pending = false;
    t->kind = kind;
    memcpy(t->base, topic, slash - topic);
    t->base[slash - topic] = 0;
    parse_mac3(t->base, t->mac3);
    remember_model(kind, t->mac3, t->base);
    s_links_dirty = true;
    unlock();
    ESP_LOGI(TAG, "링크 = f0%d (%s) mac %02x%02x%02x", kind, t->base, t->mac3[0], t->mac3[1], t->mac3[2]);
    kick();
}

/* 단독 모드: 기기의 시간 요청(stg, 약 1시간마다)에 클라우드처럼 set 72 = 현재 한국 시각으로 답한다
 * (캡처: 클라우드가 40ms 안에 {"rpc":[{"id":"72","data":"YYYYMMDDTHHMMSS"}]} 를 <base>/set 으로 보냄). */
static void answer_time(hub_link_tap_t *t)
{
    time_t now = time(NULL);
    if (now < 1704067200) {                          /* 시계가 아직 안 맞음(SNTP 전): 틀린 시각은 안 준다 */
        ESP_LOGW(TAG, "단독: 기기 시간 요청 — 허브 시계가 아직 안 맞아 건너뜀");
        return;
    }
    now += 9 * 3600;                                 /* 한국 시각(UTC+9) */
    struct tm tm;
    gmtime_r(&now, &tm);
    char ts[20], topic[80], json[160];
    strftime(ts, sizeof(ts), "%Y%m%dT%H%M%S", &tm);
    int tl = snprintf(topic, sizeof(topic), "%s/set", t->base);
    int j = snprintf(json, sizeof(json),
        "{\"ptn\":\"J07\",\"code\":\"null\",\"id\":\"f0%d\",\"no\":\"1\",\"sum\":1,"
        "\"rpc\":[{\"id\":\"72\",\"data\":\"%s\"}],\"lt\":\"null\"}", t->kind, ts);
    if (tl > 0 && tl < (int)sizeof(topic) && j > 0 && j < (int)sizeof(json) && enqueue_pub(t, topic, tl, json, j, HUB_TR_SRC_TIME)) {
        ESP_LOGI(TAG, "단독: f0%d 시간 알려 줌 %s", t->kind, ts);
    }
}

static void on_down_packet(hub_link_tap_t *t, const uint8_t *p, int n, int h)
{
    if ((p[0] >> 4) == 8 && n >= h + 4) {            /* SUBSCRIBE: [pid 2][토픽 길이 2][토픽][qos] */
        int tl = (p[h + 2] << 8) | p[h + 3];
        if (tl > 0 && tl < 96 && h + 4 + tl <= n) {
            char topic[96];
            memcpy(topic, p + h + 4, tl);
            topic[tl] = 0;
            const char *slash = strrchr(topic, '/');
            if (slash && (!strcmp(slash, "/set") || !strcmp(slash, "/get"))) {
                learn_base(t, topic, slash);
                if (!strcmp(slash, "/get") && hub_proxy_local()) {
                    t->poll_now = true;              /* 클라우드는 접속 ~6초 뒤 fa 를 보냄 — 단독은 허브가 바로 */
                    kick();
                }
            }
        }
        return;
    }
    if ((p[0] >> 4) != 3 || n < h + 2) {
        return;
    }
    /* 기기가 올린 PUBLISH 를 시각·순번과 함께 기록(GET /api/v1/trace). 첫 패킷은 아직 종류를 모를 수 있다(토픽에 있음) */
    uint32_t tseq = hub_trace_add(HUB_TR_DEV2CLOUD, HUB_TR_SRC_DEVICE, t->kind, t->mac3, p, n, h);
    int qos = (p[0] >> 1) & 3;
    int tl = (p[h] << 8) | p[h + 1];
    int off = h + 2 + tl + (qos ? 2 : 0);
    if (off > n || tl <= 0 || tl >= (int)sizeof(t->base) + 8) {
        return;
    }
    char topic[96];
    memcpy(topic, p + h + 2, tl);
    topic[tl] = 0;
    const char *slash = strrchr(topic, '/');
    if (slash && !strcmp(slash, "/stg")) {
        learn_base(t, topic, slash);
        if (t->kind && hub_proxy_local()) {
            answer_time(t);
        }
        return;
    }
    if (!slash || (strcmp(slash, "/inf") && strcmp(slash, "/res"))) {
        return;
    }
    learn_base(t, topic, slash);
    uint8_t kind = t->kind;
    if (!kind) {
        return;
    }

    /* "id":"70","data":"<hex>" */
    const char *key = "\"id\":\"70\",\"data\":\"";
    int kl = strlen(key), plen = n - off;
    const uint8_t *pl = p + off;
    for (int i = 0; i + kl < plen; i++) {
        if (memcmp(pl + i, key, kl)) {
            continue;
        }
        uint8_t blob[HUB_BLOB_MAX];
        int bn = 0;
        for (int j = i + kl; j + 1 < plen && isxdigit(pl[j]) && isxdigit(pl[j + 1]) && bn < HUB_BLOB_MAX; j += 2) {
            char hx[3] = { (char)pl[j], (char)pl[j + 1], 0 };
            blob[bn++] = (uint8_t)strtoul(hx, NULL, 16);
        }
        if (!bn) {                                   /* 빈 상태("data":"") — 기기표 칸을 차지하지 않게 */
            return;
        }
        lock();
        rk_dev_t *d = dev_get(kind, t->mac3);
        bool changed = d && (d->n != bn || memcmp(d->blob, blob, bn));
        pend_report(kind, t->mac3, blob, bn);
        if (d) {
            d->seq = tseq;
            if (kind == HUB_KIND_F06 && bn > 2) {            /* 온수 예열이 켜진 때를 기억(자동 끄기) */
                if (bnib(blob, 5) != 1) {
                    if (d->pre_on_ms) {
                        s_boilcfg_dirty = true;
                    }
                    d->pre_on_ms = 0;
                    d->pre_off_at = 0;
                } else if (!d->pre_on_ms) {
                    uint64_t now = hub_trace_now_ms();
                    uint64_t lim = s_test_preoff_s ? (uint64_t)s_test_preoff_s * 1000 : (uint64_t)s_pre_off_min * 60000;
                    d->pre_on_ms = now | 1;
                    d->pre_off_at = lim ? now + lim : 0;     /* 기본값으로 */
                    d->pre_try_ms = 0;
                    s_boilcfg_dirty = true;
                    kick();
                }
            }
            if (changed) {
                d->chg_seq = tseq;
                d->chg_ms = hub_trace_now_ms();
            }
        }
        bool burn_ch = false;
        uint8_t burn_v = 0;
        int out_ev[RK_NZ], nout = 0;             /* 항목: 존<<1 | (외출?1:0) */
        if (changed) {
            memcpy(d->blob, blob, bn);
            d->n = bn;
            d->dirty = true;
            if (kind == HUB_KIND_F06 && bn > 4) {
                uint8_t nb = (uint8_t)((bnib(blob, 8) ? 1 : 0) | (bnib(blob, 9) ? 2 : 0));
                if (nb != d->burn) {
                    d->burn = nb;
                    burn_ch = true;
                    burn_v = nb;
                }
            } else if (kind == HUB_KIND_F09) {
                for (int z = 0; z < RK_NZ; z++) {
                    int st = RK_ZSTAT[z];
                    bool present = st < bn && (blob[st] || (RK_ZCUR[z] < bn && blob[RK_ZCUR[z]]));
                    uint8_t bit = (present && (blob[st] & 0x10)) ? (uint8_t)(1 << z) : 0;
                    if (bit != (d->out_mask & (1 << z))) {
                        d->out_mask = (uint8_t)((d->out_mask & ~(1 << z)) | bit);
                        out_ev[nout++] = (z << 1) | (bit ? 1 : 0);
                    }
                }
            }
        }
        unlock();
        if (changed) {
            ESP_LOGI(TAG, "f0%d %02x%02x%02x 상태 바뀜 (%d바이트)", kind, t->mac3[0], t->mac3[1], t->mac3[2], bn);
            uint8_t m6[6] = { t->mac3[0], t->mac3[1], t->mac3[2], 0, 0, 0 };   /* 이벤트 mac 은 6바이트 */
            if (burn_ch) {
                gas_view_t gv;
                uint32_t step;
                uint32_t pulses = hub_sensors_gas(&gv, &step) ? gv.count : 0;
                hub_event(EV_BOIL_BURN, m6, burn_v, (int32_t)pulses);   /* 가스↔연소 상관용 */
            }
            for (int k = 0; k < nout; k++) {
                hub_event(EV_ROOM_OUT, m6, out_ev[k] >> 1, out_ev[k] & 1);
            }
            kick();
        }
        return;
    }
}

/* ---------------------------------------------------------------- 링크 */

hub_link_tap_t *hub_rinnai_link_open(uint32_t peer_ip)
{
    lock();
    hub_link_tap_t *t = NULL;
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!s_taps[i].used) {
            t = &s_taps[i];
            break;
        }
    }
    if (t) {
        memset(&t->down, 0, sizeof(t->down));
        memset(&t->up, 0, sizeof(t->up));
        t->down.pkt = malloc(CAP_DOWN);
        t->down.cap = t->down.pkt ? CAP_DOWN : 0;
        t->up.pkt = malloc(CAP_UP);                  /* 클라우드→기기 패킷도 모아 기록한다(없으면 경계만 추적) */
        t->up.cap = t->up.pkt ? CAP_UP : 0;
        t->kind = 0;
        t->base[0] = 0;
        memset(t->mac3, 0, 3);
        t->peer_ip = peer_ip;
        t->refused = false;
        t->pending = false;
        if (!t->cmds) {
            t->cmds = xQueueCreate(16, sizeof(cmd_t *));   /* 방 7개에 한꺼번에(외출 등) + 여유 */
        }
        t->used = true;
    }
    unlock();
    return t;
}

void hub_rinnai_link_close(hub_link_tap_t *t)
{
    if (!t) {
        return;
    }
    lock();
    t->used = false;
    t->kind = 0;
    s_links_dirty = true;
    free(t->down.pkt);
    t->down.pkt = NULL;
    free(t->up.pkt);
    t->up.pkt = NULL;
    t->up.cap = 0;
    cmd_t *c;
    while (t->cmds && xQueueReceive(t->cmds, &c, 0) == pdTRUE) {
        free(c);
    }
    unlock();
    kick();
}

/* 클라우드가 기기로 보낸 PUBLISH(린나이 앱 조작·클라우드의 상태 요청 등)를 기록 */
static void on_up_packet(hub_link_tap_t *t, const uint8_t *p, int n, int h)
{
    hub_trace_add(HUB_TR_CLOUD2DEV, HUB_TR_SRC_CLOUD, t->kind, t->mac3, p, n, h);
}

void hub_rinnai_tap_down(hub_link_tap_t *t, const uint8_t *data, int n)
{
    if (t) {
        framer_feed(t, &t->down, data, n, on_down_packet);
    }
}

void hub_rinnai_tap_up(hub_link_tap_t *t, const uint8_t *data, int n)
{
    if (t) {
        framer_feed(t, &t->up, data, n, on_up_packet);
    }
}

int hub_rinnai_take_cmd(hub_link_tap_t *t, uint8_t *out, int cap)
{
    cmd_t *c;
    if (!t || !t->cmds || !framer_at_boundary(&t->up) || xQueueReceive(t->cmds, &c, 0) != pdTRUE) {
        return 0;
    }
    int n = c->len <= cap ? c->len : 0;
    if (n && c->pend >= 0 && s_test_drop) {          /* 시험: 보낸 것으로 치고 버린다(명령이 사라진 경우를 흉내) */
        s_test_drop--;
        ESP_LOGW(TAG, "시험: 명령 하나를 기기로 보내지 않음");
        lock();
        if (s_pend[c->pend].used && s_pend[c->pend].gen == c->pend_gen) {
            s_pend[c->pend].sent_ms = hub_trace_now_ms() | 1;
        }
        unlock();
        free(c);
        kick();
        return 0;
    }
    memcpy(out, c->pkt, n);
    if (n > 2) {                                     /* 지금 기기 쪽 소켓에 써 넣는다 — 이 순간을 기록 */
        hub_trace_add(HUB_TR_HUB2DEV, c->src, t->kind, t->mac3, c->pkt, n, (c->pkt[1] & 0x80) ? 3 : 2);
    }
    if (c->pend >= 0 && c->pend < PEND_MAX) {        /* 확인을 기다리는 명령: 여기서부터 2.5초를 잰다 */
        lock();
        if (s_pend[c->pend].used && s_pend[c->pend].gen == c->pend_gen) {
            s_pend[c->pend].sent_ms = n > 2 ? hub_trace_now_ms() | 1 : 0;
        }
        unlock();
        kick();
    }
    free(c);
    return n;
}

/* ---------------------------------------------------------------- 명령 */

static bool token_ok(const char *s, int n, int max)
{
    if (n <= 0 || n > max) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (!isalnum((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

/* 기기에 보내도 되는 쓰기 코드(드라이버가 쓰는 것, 실기로 확인한 것). 새 기능은 여기에 더한다.
 *   80 전원  b1 난방  b3 외출  b4 난방 방식  d1 온수 온도  df (d1 과 같이)  d2 난방수 온도  d4 실내 온도
 *   a0 각방 번호  96 예약 켜기  97 예약 간격  da 시간 난방 분(1시간에 N분, 린나이 앱 5·10·20…60) */
/* 드라이버(edge-driver/src/core.lua)가 보내는 코드와 같아야 한다 — 드라이버 시험(sim.lua)이 이 줄을 읽어 대조한다 */
static bool code_allowed(const char *c)
{
    static const char *const OK[] = { "80", "b1", "b3", "b4", "b6", "d1", "d7", "df", "d2", "d4", "a0", "96", "97", "da" };
    for (size_t i = 0; i < sizeof(OK) / sizeof(OK[0]); i++) {
        if (!strcmp(c, OK[i])) {
            return true;
        }
    }
    return false;
}

static void command_one(const char *text, int len);

/* "f06@aabbcc 80=1;f09@ddeeff a0=7,b1=1" — ';' 로 여러 개, '@MAC끝3바이트' 로 대상 지정(생략 가능) */
void hub_rinnai_command(const char *text, int len)
{
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || text[i] == ';') {
            if (i > start) {
                command_one(text + start, i - start);
            }
            start = i + 1;
        }
    }
}

/* 지정된 mac3 과 kind 에 맞는 링크. mac3==NULL 이면 그 kind 의 아무 링크(단일 기기 하위호환) */
static hub_link_tap_t *find_tap(uint8_t kind, const uint8_t *mac3)
{
    hub_link_tap_t *any = NULL;
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!s_taps[i].used || s_taps[i].kind != kind) {
            continue;
        }
        if (mac3 && mac3_eq(s_taps[i].mac3, mac3)) {
            return &s_taps[i];
        }
        any = &s_taps[i];
    }
    return mac3 ? NULL : any;
}

/* PUBLISH 패킷을 만들어 tap 의 주입 큐에 넣는다(QoS0). 성공 true. 호출자가 lock 보유.
 * pend >= 0 이면 확인을 기다리는 명령(s_pend 의 그 칸) — 써 넣는 순간 hub_rinnai_take_cmd 가 시각을 적는다. */
static bool enqueue_pub2(hub_link_tap_t *t, const char *topic, int tl, const char *json, int j, uint8_t src, int pend);
static bool enqueue_pub(hub_link_tap_t *t, const char *topic, int tl, const char *json, int j, uint8_t src)
{
    return enqueue_pub2(t, topic, tl, json, j, src, -1);
}

static bool enqueue_pub2(hub_link_tap_t *t, const char *topic, int tl, const char *json, int j, uint8_t src, int pend)
{
    cmd_t *c = malloc(sizeof(cmd_t));
    if (c) {
        c->src = src;
        c->pend = (int8_t)pend;
        c->pend_gen = pend >= 0 ? s_pend[pend].gen : 0;
    }
    int rl = 2 + tl + j;
    if (!c || rl > 16383 || 3 + rl > CMD_MAX) {
        free(c);
        return false;
    }
    uint8_t *q = c->pkt;
    *q++ = 0x30;                                       /* PUBLISH, QoS 0 */
    if (rl < 128) {
        *q++ = rl;
    } else {
        *q++ = (rl & 0x7f) | 0x80;
        *q++ = rl >> 7;
    }
    *q++ = tl >> 8;
    *q++ = tl & 0xff;
    memcpy(q, topic, tl);
    q += tl;
    memcpy(q, json, j);
    q += j;
    c->len = q - c->pkt;
    if (xQueueSend(t->cmds, &c, 0) != pdTRUE) {
        free(c);
        return false;
    }
    return true;
}

/* 모든 연결에 상태 요청(fa)을 주입한다. 보일러가 상태(70)를 자주 안 올려서, 외출 등 변경이
 * 빨리 반영되도록 주기적으로 당긴다(Pi 기록기가 1분마다 하던 것). topic 은 <base>/get. */
static void poll_status(bool all)
{
    lock();
    for (int i = 0; i < MAX_TAPS; i++) {
        if (!s_taps[i].used || !s_taps[i].kind || (!all && !s_taps[i].poll_now)) {
            continue;
        }
        s_taps[i].poll_now = false;
        char topic[80];
        int tl = snprintf(topic, sizeof(topic), "%s/get", s_taps[i].base);
        char json[128];
        int j = snprintf(json, sizeof(json),
            "{\"ptn\":\"J07\",\"code\":\"null\",\"id\":\"f0%d\",\"no\":\"1\",\"sum\":1,\"rpc\":[{\"id\":\"fa\"}],\"lt\":\"null\"}",
            s_taps[i].kind);
        if (tl > 0 && tl < (int)sizeof(topic) && j > 0 && j < (int)sizeof(json)) {
            enqueue_pub(&s_taps[i], topic, tl, json, j, HUB_TR_SRC_POLL);
        }
    }
    unlock();
}

/* 명령 하나를 기기 쪽에 넣고, 반영되는지 지켜본다. 호출자가 lock 보유. 큐에 넣었으면 true.
 * 지금 상태가 이미 그 값이면 지켜보지 않는다(확인할 것이 없다). */
static bool send_tracked(hub_link_tap_t *t, const char *json, int j, const chk_t *chk, int nchk, const char *key, uint8_t src)
{
    char topic[80];
    int tl = snprintf(topic, sizeof(topic), "%s/set", t->base);
    if (tl <= 0 || tl >= (int)sizeof(topic)) {
        return false;
    }
    int slot = -1;
    for (int i = 0; i < PEND_MAX; i++) {             /* 같은 칸의 옛 명령은 잊는다 */
        pend_t *q = &s_pend[i];
        if (q->used && q->kind == t->kind && mac3_eq(q->mac3, t->mac3) && !strcmp(q->key, key)) {
            q->used = false;
        }
    }
    rk_dev_t *d = dev_find(t->kind, t->mac3);
    bool already = nchk > 0 && d && d->n && chk_all(chk, nchk, d->blob, d->n);
    if (!already && j < (int)sizeof(s_pend[0].json)) {
        for (int i = 0; i < PEND_MAX && slot < 0; i++) {
            if (!s_pend[i].used) {
                slot = i;
            }
        }
    }
    if (slot >= 0) {
        pend_t *q = &s_pend[slot];
        memset(q, 0, sizeof(*q));
        q->kind = t->kind;
        memcpy(q->mac3, t->mac3, 3);
        q->tries = 1;
        q->src = src;
        q->nchk = (uint8_t)nchk;
        memcpy(q->chk, chk, sizeof(chk_t) * nchk);
        q->gen = ++s_pend_gen;
        q->jlen = (uint16_t)j;
        memcpy(q->json, json, j);
        strlcpy(q->key, key, sizeof(q->key));
        q->made_ms = hub_trace_now_ms();
        q->used = true;
    }
    if (!enqueue_pub2(t, topic, tl, json, j, src, slot)) {
        if (slot >= 0) {
            s_pend[slot].used = false;
        }
        return false;
    }
    if (slot >= 0) {
        s_cmd_tracked++;
    }
    return true;
}

/* 기다리는 명령들을 본다: 2.5초 안에 반영이 없으면 다시 보내고, 3번 보내도 없으면 포기. 남은 것이 있으면 true. */
static bool pend_tick(void)
{
    bool left = false;
    uint64_t now = hub_trace_now_ms();
    lock();
    for (int i = 0; i < PEND_MAX; i++) {
        pend_t *q = &s_pend[i];
        if (!q->used) {
            continue;
        }
        hub_link_tap_t *t = find_tap(q->kind, q->mac3);
        if (!t) {                                    /* 연결이 끊겼다 — 다시 붙으면 기기가 자기 상태를 올린다 */
            q->used = false;
            continue;
        }
        if (!q->sent_ms) {
            if (now - q->made_ms > PEND_QUEUE_MS) {
                q->used = false;
            }
        } else if (now - q->sent_ms >= PEND_WAIT_MS) {
            char topic[80];
            int tl = snprintf(topic, sizeof(topic), "%s/set", t->base);
            if (q->tries >= PEND_TRIES) {
                s_cmd_failed++;
                snprintf(s_cmd_last_fail, sizeof(s_cmd_last_fail), "f0%d %s", q->kind, q->key);
                ESP_LOGW(TAG, "명령이 반영되지 않음 — f0%d %02x%02x%02x %s(%d번 보냄, 포기)",
                         q->kind, q->mac3[0], q->mac3[1], q->mac3[2], q->key, q->tries);
                q->used = false;
            } else if (tl > 0 && tl < (int)sizeof(topic) &&
                       enqueue_pub2(t, topic, tl, q->json, q->jlen, HUB_TR_SRC_RETRY, i)) {
                q->tries++;
                q->sent_ms = 0;
                q->made_ms = now;
                s_cmd_resent++;
                ESP_LOGW(TAG, "명령에 답이 없어 다시 보냄(%d번째) — f0%d %02x%02x%02x %s",
                         q->tries, q->kind, q->mac3[0], q->mac3[1], q->mac3[2], q->key);
            }
        }
        left = left || q->used;
    }
    unlock();
    return left;
}

/* 온수 예열 자동 끄기: 켜진 것을 본 지 정한 시간이 지나면 끈다(어디서 켰든). 예열은 스스로 꺼지지 않고, 켜 둔 동안
 * 10분쯤마다 버너가 돈다(2026-11-23 실측). 한 번 보낸 뒤 30초가 지나도 켜져 있으면 다시 시도한다. */
static void preheat_tick(void)
{
    uint64_t now = hub_trace_now_ms();
    lock();
    for (int i = 0; i < MAX_DEV; i++) {
        rk_dev_t *d = &s_dev[i];
        if (!d->used || d->kind != HUB_KIND_F06 || !d->pre_on_ms || !d->pre_off_at || now < d->pre_off_at ||
            (d->pre_try_ms && now - d->pre_try_ms < 30000)) {
            continue;
        }
        hub_link_tap_t *t = find_tap(HUB_KIND_F06, d->mac3);
        if (!t) {
            continue;
        }
        static const char json[] = "{\"ptn\":\"J07\",\"code\":\"null\",\"id\":\"f06\",\"no\":\"1\",\"sum\":1,"
                                   "\"rpc\":[{\"id\":\"b6\",\"data\":\"0\"}],\"lt\":\"null\"}";
        const chk_t chk = { .type = CHK_NIB, .pos = 5, .want = 0 };
        d->pre_try_ms = now | 1;
        if (send_tracked(t, json, sizeof(json) - 1, &chk, 1, "b6,", HUB_TR_SRC_AUTO)) {
            ESP_LOGW(TAG, "온수 예열 자동 끄기(켜진 지 %lu초) — f06 %02x%02x%02x", (unsigned long)((now - d->pre_on_ms) / 1000),
                     d->mac3[0], d->mac3[1], d->mac3[2]);
        }
    }
    unlock();
}

/* 예열이 켜진 첫 보일러의 남은 시간: 0 = 꺼짐, 1~60 = 남은 분(올림), 255 = 켜짐·무제한. 잠근 채로 부른다 */
static uint8_t preheat_left_locked(uint64_t now, uint32_t *left_s)
{
    for (int i = 0; i < MAX_DEV; i++) {
        const rk_dev_t *d = &s_dev[i];
        if (d->used && d->kind == HUB_KIND_F06 && d->pre_on_ms) {
            if (!d->pre_off_at) {
                if (left_s) {
                    *left_s = UINT32_MAX;
                }
                return 255;
            }
            uint64_t left = d->pre_off_at > now ? d->pre_off_at - now : 0;
            if (left_s) {
                *left_s = (uint32_t)(left / 1000);
            }
            uint64_t m = (left + 59999) / 60000;
            return (uint8_t)(m < 1 ? 1 : m > 60 ? 60 : m);
        }
    }
    return 0;
}

static void command_one(const char *text, int len)
{
    char s[128];
    if (len <= 0 || len >= (int)sizeof(s)) {
        return;
    }
    memcpy(s, text, len);
    s[len] = 0;
    if (!strcmp(s, "refresh")) {
        /* 여기는 Zigbee 스택 잠금을 쥔 콜백 안일 수 있다 — 표시만 하고 보내기는 zb_task 가 한다.
         * (노출 잠금을 여기서 잡으면, 노출 잠금을 쥐고 Zigbee 로 보내려는 다른 태스크와 서로 기다린다) */
        lock();
        for (int i = 0; i < MAX_DEV; i++) {
            if (s_dev[i].used) {       /* 연결된 기기만 — 끊긴 기기의 옛 상태를 보내면 드라이버가 온라인으로 본다 */
                s_dev[i].dirty = s_dev[i].n > 0 && find_tap(s_dev[i].kind, s_dev[i].mac3) != NULL;
            }
        }
        s_links_dirty = true;
        s_expose_dirty = true;         /* 노출 마스크도 다시(드라이버 재시작 후 비트 재동기화) */
        unlock();
        kick();
        return;
    }
    uint8_t kind = !strncmp(s, "f06", 3) ? HUB_KIND_F06 : !strncmp(s, "f09", 3) ? HUB_KIND_F09 : 0;
    if (!kind) {
        ESP_LOGW(TAG, "모르는 명령: %s", s);
        return;
    }
    /* 선택적 '@<6 hex>' 대상 MAC */
    char *rest = s + 3;
    uint8_t mac3[3];
    const uint8_t *target = NULL;
    if (*rest == '@') {
        if (token_ok(rest + 1, 6, 6)) {
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%.6s", rest + 1);
            for (int i = 0; i < 3; i++) {
                char hx[3] = { tmp[2 * i], tmp[2 * i + 1], 0 };
                mac3[i] = (uint8_t)strtoul(hx, NULL, 16);
            }
            target = mac3;
        } else {                                     /* @ 뒤가 MAC 이 아니면 아무 기기로 보내지 않고 버린다 */
            ESP_LOGW(TAG, "대상 MAC 형식 틀림 — 버림: %s", s);
            return;
        }
        rest = strchr(rest, ' ');
        if (!rest) {
            return;
        }
    }
    while (*rest == ' ') {
        rest++;
    }

    /* 클라우드가 보내는 모양 그대로: "sum" = rpc 항목 수, 값은 소문자 16진(예 "1c") */
    for (char *c = rest; *c; c++) {
        *c = tolower((unsigned char)*c);
    }
    char rpc[192];
    int r = 0, items = 0;
    char codes[8][3];                                /* 확인할 칸을 뽑으려고 항목을 따로 적어 둔다(8개까지) */
    uint32_t vals[8];
    for (char *save = NULL, *tok = strtok_r(rest, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq || !token_ok(tok, eq - tok, 2) || !token_ok(eq + 1, strlen(eq + 1), 16)) {
            ESP_LOGW(TAG, "잘못된 명령 항목: %s", tok);
            return;
        }
        *eq = 0;
        if (!code_allowed(tok)) {                    /* 확인한 쓰기 코드만(검증 안 된 코드로 보일러를 건드리지 않게) */
            ESP_LOGW(TAG, "허용 안 된 코드 %s — 명령 버림", tok);
            return;
        }
        r += snprintf(rpc + r, sizeof(rpc) - r, "%s{\"id\":\"%s\",\"data\":\"%s\"}", items ? "," : "", tok, eq + 1);
        if (items < 8) {
            strlcpy(codes[items], tok, sizeof(codes[0]));
            vals[items] = (uint32_t)strtoul(eq + 1, NULL, 16);
        }
        items++;
        if (r >= (int)sizeof(rpc) - 8) {
            return;
        }
    }
    if (!items) {
        return;
    }
    char json[256];
    int j = snprintf(json, sizeof(json), "{\"ptn\":\"J07\",\"code\":\"null\",\"id\":\"f0%d\",\"no\":\"1\",\"sum\":%d,\"rpc\":[%s],\"lt\":\"null\"}",
                     kind, items, rpc);
    if (j >= (int)sizeof(json)) {
        return;
    }

    lock();
    hub_link_tap_t *t = find_tap(kind, target);
    if (!t) {
        unlock();
        ESP_LOGW(TAG, "f0%d 연결 없음 — 명령 버림: %s", kind, s);
        return;
    }
    chk_t chk[CHK_MAX];
    char key[24];
    int nchk = build_checks(kind, codes, vals, items < 8 ? items : 8, chk, key, sizeof(key));
    bool ok = send_tracked(t, json, j, chk, nchk, key, HUB_TR_SRC_PLATFORM);
    unlock();
    if (ok) {
        ESP_LOGI(TAG, "명령 → f0%d %s", kind, json);
        kick();                                      /* 확인 시계를 돌리게 태스크를 깨운다 */
    } else {
        ESP_LOGW(TAG, "명령 큐 가득/실패");
    }
}

/* ---------------------------------------------------------------- 플랫폼으로 내보내기 */

/* 연결 목록: 활성 링크마다 [kind][mac3]. 드라이버가 목록에 없는 기기를 오프라인 처리. */
static int build_links(uint8_t *out, int cap)
{
    int n = 0;
    for (int i = 0; i < MAX_TAPS && n + 4 <= cap; i++) {
        if (s_taps[i].used && s_taps[i].kind) {
            out[n++] = s_taps[i].kind;
            memcpy(out + n, s_taps[i].mac3, 3);
            n += 3;
        }
    }
    return n;
}

/* 연결된 기기의 모델코드: ([종류][MAC끝3][모델]) * n — 모르는 기기는 뺀다 */
static int build_models(uint8_t *out, int cap)
{
    int n = 0;
    for (int i = 0; i < MAX_TAPS && n + 5 <= cap; i++) {
        if (!s_taps[i].used || !s_taps[i].kind) {
            continue;
        }
        int m = model_of(s_taps[i].kind, s_taps[i].mac3);
        if (m >= 0) {
            out[n++] = s_taps[i].kind;
            memcpy(out + n, s_taps[i].mac3, 3);
            n += 3;
            out[n++] = (uint8_t)m;
        }
    }
    return n;
}

static void send_boilcfg(void);

static void zb_task(void *arg)
{
    TickType_t last_beat = xTaskGetTickCount();
    TickType_t last_poll = xTaskGetTickCount();
    TickType_t last_alive = xTaskGetTickCount();
    bool waiting = false;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waiting ? 250 : 10000));   /* 확인을 기다리는 명령이 있으면 촘촘히 */
        waiting = pend_tick();
        preheat_tick();
        {   /* 온수 예열 남은 시간: 켜지고 꺼질 때 바로, 켜져 있는 동안 1분마다 알린다 */
            static TickType_t last_left;
            lock();
            bool on = preheat_left_locked(hub_trace_now_ms(), NULL) != 0;
            unlock();
            if (s_boilcfg_dirty || (on && xTaskGetTickCount() - last_left >= pdMS_TO_TICKS(60000))) {
                s_boilcfg_dirty = false;
                last_left = xTaskGetTickCount();
                send_boilcfg();
            }
        }
        if (xTaskGetTickCount() - last_alive >= pdMS_TO_TICKS(ALIVE_S * 1000)) {
            /* SmartThings 는 Zigbee 기기가 30분쯤 조용해야 오프라인으로 바꾼다. 드라이버가 먼저 알아채도록 짧게 자주 */
            /* [0][칩 온도 s16, 1/100°C] — 온도는 그 통로의 노출 설정에서 켰을 때만 붙는다(없으면 1바이트) */
            static const struct { int ch; uint8_t uplink; } ways[] = {
                { EXPOSE_CH_ZIGBEE, HUB_UPLINK_ZIGBEE }, { EXPOSE_CH_LAN, HUB_UPLINK_LAN }, { EXPOSE_CH_MATTER, HUB_UPLINK_MATTER } };
            last_alive = xTaskGetTickCount();
            for (int w = 0; w < 3; w++) {
                uint8_t alive[3] = { 0 };
                int16_t chip;
                bool has_chip = hub_main_chip_temp(ways[w].ch, &chip);
                if (has_chip) {
                    memcpy(alive + 1, &chip, 2);
                }
                hub_uplink_send_to(ways[w].uplink, HUB_RK_CMD_ALIVE, alive, has_chip ? 3 : 1);
            }
        }
        bool beat = xTaskGetTickCount() - last_beat >= pdMS_TO_TICKS(HEARTBEAT_S * 1000);
        lock();
        bool expose = s_expose_dirty;
        s_expose_dirty = false;
        unlock();
        if (beat) {
            last_beat = xTaskGetTickCount();
        }
        if (beat || expose) {
            hub_expose_send_all();     /* 주기적으로 + refresh 때 노출 마스크도(드라이버 재시작 후 동기화) */
        }
        if (xTaskGetTickCount() - last_poll >= pdMS_TO_TICKS(POLL_S * 1000)) {
            last_poll = xTaskGetTickCount();
            poll_status(true);          /* 주기적 상태 요청(fa) — 외출 등 변경 빨리 반영 */
        } else {
            poll_status(false);         /* 단독 모드에서 막 구독한 기기 */
        }
        /* 연결 목록 */
        uint8_t links[MAX_TAPS * 4], models[MAX_TAPS * 5];
        int ln, mn;
        bool send_links;
        lock();
        ln = build_links(links, sizeof(links));
        mn = build_models(models, sizeof(models));
        send_links = s_links_dirty || beat;
        s_links_dirty = false;
        unlock();
        if (send_links) {
            ESP_LOGI(TAG, "→ 연결 %d대", ln / 4);
            hub_uplink_send(HUB_RK_CMD_LINKS, links, ln);
            if (mn) {
                hub_uplink_send(HUB_RK_CMD_MODELS, models, mn);   /* 온도 표 고르기용(옛 드라이버는 무시) */
            }
            uint8_t pairs[MAX_PAIRS_BYTES];
            int pn = hub_pairs_blob(pairs, sizeof(pairs));
            hub_uplink_send(HUB_RK_CMD_PAIRS, pairs, pn);   /* 짝(없으면 0바이트 = 전부 단독/암묵) */
            send_boilcfg();                                  /* 보일러 설정(온수 예열 자동 끄기) */
        }
        /* 기기별 상태 */
        for (int i = 0; i < MAX_DEV; i++) {
            uint8_t kind, mac3[3], blob[HUB_BLOB_MAX];
            int bn = 0;
            lock();
            /* 주기 재전송은 연결된 기기만 — 끊긴 기기의 옛 상태를 보내면 드라이버가 온라인으로 되돌렸다 다시 끊김 처리(깜빡임) */
            if (s_dev[i].used && s_dev[i].n &&
                (s_dev[i].dirty || (beat && find_tap(s_dev[i].kind, s_dev[i].mac3)))) {
                kind = s_dev[i].kind;
                memcpy(mac3, s_dev[i].mac3, 3);
                bn = s_dev[i].n;
                memcpy(blob, s_dev[i].blob, bn);
                s_dev[i].dirty = false;
            }
            unlock();
            if (bn) {
                vTaskDelay(pdMS_TO_TICKS(200));
                uint8_t msg[1 + 3 + HUB_BLOB_MAX];
                msg[0] = kind;
                memcpy(msg + 1, mac3, 3);
                memcpy(msg + 4, blob, bn);
                ESP_LOGI(TAG, "→ f0%d %02x%02x%02x 상태 %d바이트", kind, mac3[0], mac3[1], mac3[2], bn);
                hub_uplink_send(HUB_RK_CMD_STATE, msg, bn + 4);
            }
        }
    }
}

int hub_rinnai_snapshot(hub_dev_t *out, int max)
{
    int n = 0;
    if (!s_lock) {
        return 0;
    }
    lock();
    for (int i = 0; i < MAX_DEV && n < max; i++) {
        if (!s_dev[i].used || !s_dev[i].n) {
            continue;
        }
        out[n].kind = s_dev[i].kind;
        memcpy(out[n].mac3, s_dev[i].mac3, 3);
        out[n].n = s_dev[i].n;
        memcpy(out[n].blob, s_dev[i].blob, s_dev[i].n);
        out[n].linked = find_tap(s_dev[i].kind, s_dev[i].mac3) != NULL;
        out[n].seq = s_dev[i].seq;
        out[n].chg_seq = s_dev[i].chg_seq;
        out[n].chg_ms = s_dev[i].chg_ms;
        out[n].model = (int16_t)model_of(s_dev[i].kind, s_dev[i].mac3);
        n++;
    }
    unlock();
    return n;
}

/* 관리 화면에서 짝이 바뀌면: 연결(과 짝)을 다시 보내도록 깨운다 */
void hub_rinnai_resend(void)
{
    if (!s_lock) {
        return;
    }
    lock();
    s_links_dirty = true;
    unlock();
    kick();
}

/* 관리 화면: 기기 삭제. 허브 기기표(s_dev)와 등록 목록에서 지우고, 드라이버에 CMD_FORGET 으로 알린다.
 * 지금 연결돼 있는 기기면 그 연결은 '등록 안 된 단말'이 되어 관리 화면 "등록 대기"에 나온다(다시 등록할 수 있다). */
bool hub_rinnai_forget(const char *mac6)
{
    if (!s_lock || !mac6) {
        return false;
    }
    uint8_t m[3];
    for (int i = 0; i < 3; i++) {
        char hx[3] = { mac6[2 * i], mac6[2 * i + 1], 0 };
        m[i] = (uint8_t)strtoul(hx, NULL, 16);
    }
    uint8_t gone[MAX_DEV][4];
    int ng = 0;
    hub_devs_remove3(m);                             /* 등록 해제 — 다시 붙어도 기기로 보지 않는다 */
    lock();
    for (int i = 0; i < MAX_TAPS; i++) {             /* 지금 그 기기의 연결: 배운 것을 잊는다(다음 토픽에서 다시 판정) */
        if (s_taps[i].used && s_taps[i].kind && mac3_eq(s_taps[i].mac3, m)) {
            s_taps[i].kind = 0;
            s_taps[i].base[0] = 0;
            s_taps[i].pending = false;
            s_links_dirty = true;
        }
    }
    for (int i = 0; i < MAX_DEV; i++) {
        if (s_model[i].used && mac3_eq(s_model[i].mac3, m)) {
            s_model[i].used = false;
        }
    }
    for (int i = 0; i < MAX_DEV; i++) {
        if (s_dev[i].used && mac3_eq(s_dev[i].mac3, m)) {
            gone[ng][0] = s_dev[i].kind;
            memcpy(gone[ng] + 1, m, 3);
            ng++;
            s_dev[i].used = false;
            s_dev[i].n = 0;
            s_dev[i].dirty = false;
            s_links_dirty = true;
        }
    }
    unlock();
    for (int i = 0; i < ng; i++) {
        hub_uplink_send(HUB_RK_CMD_FORGET, gone[i], 4);   /* 드라이버가 그 기기 자식들을 지운다 */
    }
    kick();
    return ng > 0;
}

/* ---------------------------------------------------------------- 설정·관리 화면 */

static bool pre_off_valid(int v) { return v >= 0 && v <= 60; }   /* 1~60분, 0 = 무제한 */

void hub_rinnai_test_drop(int n) { s_test_drop = (uint8_t)(n < 0 ? 0 : n > 10 ? 10 : n); }
void hub_rinnai_test_preoff(int sec)
{
    s_test_preoff_s = sec < 0 ? 0 : (uint32_t)sec;
    kick();
}

void hub_rinnai_cmd_info(hub_cmd_info_t *o)
{
    memset(o, 0, sizeof(*o));
    o->preheat_off_min = s_pre_off_min;
    o->boil_mode = s_boil_mode;
    if (!s_lock) {
        return;
    }
    uint64_t now = hub_trace_now_ms();
    lock();
    o->tracked = s_cmd_tracked;
    o->resent = s_cmd_resent;
    o->failed = s_cmd_failed;
    strlcpy(o->last_fail, s_cmd_last_fail, sizeof(o->last_fail));
    for (int i = 0; i < PEND_MAX; i++) {
        o->pending += s_pend[i].used ? 1 : 0;
    }
    preheat_left_locked(now, &o->preheat_left_s);
    for (int i = 0; i < MAX_DEV; i++) {              /* 예열이 켜져 있는 첫 보일러: 켜진 지 몇 초 */
        if (s_dev[i].used && s_dev[i].kind == HUB_KIND_F06 && s_dev[i].pre_on_ms && !o->preheat_on_s) {
            o->preheat_on_s = (uint32_t)((now - s_dev[i].pre_on_ms) / 1000) + 1;
        }
    }
    unlock();
}

static const char *const BOIL_MODE_NAME[] = { "정하지 않음", "난방수 모드", "온돌·실내 모드" };

void hub_rinnai_render(char *o, size_t cap)
{
    hub_cmd_info_t ci;
    hub_rinnai_cmd_info(&ci);
    /* 보일러 모드는 '연결된 기기'의 보일러 조절기 줄 아래(hub_pairs.c)에서 바꾼다(사용자 결정 2026-11-23).
     * 온수 예열 자동 끄기는 스마트싱스·HA 의 보일러 기기에서만 정한다(관리 화면에서는 뺌 — 사용자 결정 2026-11-23). */
    strlcat(o, "<h2>보일러 명령</h2><div class=card><p class=muted>", cap);
    char line[200];
    snprintf(line, sizeof(line), "명령 확인: 보낸 명령이 2.5초 안에 상태에 나타나지 않으면 다시 보냅니다(3번까지). "
             "켜진 뒤 다시 보냄 %lu번 · 끝내 반영 안 됨 %lu번", (unsigned long)ci.resent, (unsigned long)ci.failed);
    strlcat(o, line, cap);
    if (ci.last_fail[0]) {
        strlcat(o, " (마지막: <code>", cap);
        strlcat(o, ci.last_fail, cap);
        strlcat(o, "</code>)", cap);
    }
    strlcat(o, "</p></div>", cap);
}

/* [자동 끄기 기본값(분, 0 = 무제한)][보일러 모드 0 정하지 않음/1 난방수/2 온돌·실내][지금: 0 예열 꺼짐, 1~60 남은 분, 255 켜짐·무제한] */
static void send_boilcfg(void)
{
    uint8_t p[3] = { s_pre_off_min, s_boil_mode, 0 };
    if (s_lock) {
        lock();
        p[2] = preheat_left_locked(hub_trace_now_ms(), NULL);
        unlock();
    }
    hub_uplink_send(HUB_RK_CMD_BOILCFG, p, sizeof(p));
}

static esp_err_t save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("rinnai", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = hs_set_u8(h, key, v);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "보일러 설정 %s 저장 실패: %s", key, esp_err_to_name(e));
    }
    return e;
}

static esp_err_t pre_off_set(uint8_t m)
{
    if (m == s_pre_off_min) {
        return ESP_OK;
    }
    esp_err_t e = save_u8("pre_off", m);
    if (e != ESP_OK) {
        return e;
    }
    s_pre_off_min = m;
    ESP_LOGI(TAG, "온수 예열 자동 끄기 → %d분(0 = 무제한)", m);
    kick();
    return ESP_OK;
}

/* 보일러 모드(1 난방수 / 2 온돌·실내). 초기 설정 화면과 관리 화면에서 부른다 */
esp_err_t hub_rinnai_set_boil_mode(uint8_t m)
{
    if (m < 1 || m > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (m == s_boil_mode) {
        return ESP_OK;
    }
    esp_err_t e = save_u8("boil_mode", m);
    if (e != ESP_OK) {
        return e;
    }
    s_boil_mode = m;
    ESP_LOGI(TAG, "보일러 모드 → %s", BOIL_MODE_NAME[m]);
    send_boilcfg();
    return ESP_OK;
}

uint8_t hub_rinnai_boil_mode(void) { return s_boil_mode; }

/* 스마트홈 쪽 요청 "preoff=<분>". 바뀌었든 아니든 지금 값을 다시 알려 화면을 맞춘다(잘못된 값이면 되돌아간다). */
void hub_rinnai_request(const char *text, int len)
{
    char t[24];
    if (len > 0 && len < (int)sizeof(t)) {
        memcpy(t, text, len);
        t[len] = 0;
        if (!strncmp(t, "preoff=", 7) && isdigit((unsigned char)t[7]) && pre_off_valid(atoi(t + 7))) {
            /* 예열이 켜져 있으면 이번 예열의 남은 시간을 그 값으로(지금부터), 꺼져 있으면 다음에 켜질 때의 기본값을 바꾼다 */
            int m = atoi(t + 7);
            bool on = false;
            uint64_t now = hub_trace_now_ms();
            if (s_lock) {
                lock();
                for (int i = 0; i < MAX_DEV; i++) {
                    rk_dev_t *d = &s_dev[i];
                    if (d->used && d->kind == HUB_KIND_F06 && d->pre_on_ms) {
                        d->pre_off_at = m ? now + (uint64_t)m * 60000 : 0;
                        d->pre_try_ms = 0;
                        on = true;
                    }
                }
                unlock();
            }
            if (on) {
                ESP_LOGI(TAG, "이번 온수 예열: %s", m ? "남은 시간 다시 정함" : "무제한");
                kick();
            } else {
                pre_off_set((uint8_t)m);
            }
        } else {
            ESP_LOGW(TAG, "모르는 보일러 설정 요청: %s", t);
        }
    }
    send_boilcfg();
}

bool hub_rinnai_apply(const char *form, char *msg, size_t msglen)
{
    char v[8] = "";
    if (httpd_query_key_value(form, "bmode", v, sizeof(v)) != ESP_OK || (strcmp(v, "1") && strcmp(v, "2"))) {
        snprintf(msg, msglen, strcmp(v, "0") ? "잘못된 요청" : "보일러 모드를 골라 주세요");
        return false;
    }
    uint8_t m = (uint8_t)atoi(v);
    if (m == s_boil_mode) {
        snprintf(msg, msglen, "바뀐 것이 없습니다");
        return true;
    }
    esp_err_t e = hub_rinnai_set_boil_mode(m);
    if (e != ESP_OK) {
        snprintf(msg, msglen, "저장하지 못했습니다(%s). 설정은 그대로입니다", esp_err_to_name(e));
        return false;
    }
    snprintf(msg, msglen, "보일러 모드: %s", BOIL_MODE_NAME[m]);
    return true;
}

void hub_rinnai_start(void)
{
    if (!s_lock) {
        nvs_handle_t h;
        if (nvs_open("rinnai", NVS_READONLY, &h) == ESP_OK) {
            uint8_t m = 0;
            if (hs_get_u8(h, "rinnai", "pre_off", &m) == ESP_OK && pre_off_valid(m)) {
                s_pre_off_min = m;
            }
            if (hs_get_u8(h, "rinnai", "boil_mode", &m) == ESP_OK && m <= 2) {
                s_boil_mode = m;
            }
            nvs_close(h);
        }
        s_lock = xSemaphoreCreateMutex();
        xTaskCreate(zb_task, "hub_rk_zb", 4096, NULL, 3, &s_task);   /* 3072 일 때 최저 여유 864 — 명령 확인·재전송을 넣으며 늘림 */
    }
}
