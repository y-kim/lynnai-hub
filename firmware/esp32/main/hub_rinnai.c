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
    uint8_t out_mask;   /* f09: 마지막 존별 외출 비트 — 바뀐 존만 이벤트 */
} rk_dev_t;

/* f06 연소·f09 외출 추출용. 각방 stat/현재온도 바이트 위치(hub_api.c ZONES 와 같은 순서 z1,z2,z4,z5,z6,z7,z3) */
#define RK_NZ 7
static const uint8_t RK_ZSTAT[RK_NZ] = { 0, 7, 14, 21, 28, 35, 42 };
static const uint8_t RK_ZCUR[RK_NZ]  = { 3, 10, 17, 24, 31, 38, 45 };
static int bnib(const uint8_t *b, int i) { return i % 2 ? b[i / 2] & 0xf : b[i / 2] >> 4; }
static rk_dev_t s_dev[MAX_DEV];

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
        if (d) {
            d->seq = tseq;
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
    memcpy(out, c->pkt, n);
    if (n > 2) {                                     /* 지금 기기 쪽 소켓에 써 넣는다 — 이 순간을 기록 */
        hub_trace_add(HUB_TR_HUB2DEV, c->src, t->kind, t->mac3, c->pkt, n, (c->pkt[1] & 0x80) ? 3 : 2);
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
 *   a0 각방 번호  96 예약 켜기  97 예약 간격 */
/* 드라이버(edge-driver/src/core.lua)가 보내는 코드와 같아야 한다 — 드라이버 시험(sim.lua)이 이 줄을 읽어 대조한다 */
static bool code_allowed(const char *c)
{
    static const char *const OK[] = { "80", "b1", "b3", "b4", "b6", "d1", "d7", "df", "d2", "d4", "a0", "96", "97" };
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

/* PUBLISH 패킷을 만들어 tap 의 주입 큐에 넣는다(QoS0). 성공 true. 호출자가 lock 보유. */
static bool enqueue_pub(hub_link_tap_t *t, const char *topic, int tl, const char *json, int j, uint8_t src)
{
    cmd_t *c = malloc(sizeof(cmd_t));
    if (c) {
        c->src = src;
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
    char topic[80];
    int tl = snprintf(topic, sizeof(topic), "%s/set", t->base);
    bool ok = enqueue_pub(t, topic, tl, json, j, HUB_TR_SRC_PLATFORM);
    unlock();
    if (ok) {
        ESP_LOGI(TAG, "명령 → %s %s", topic, json);
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

static void zb_task(void *arg)
{
    TickType_t last_beat = xTaskGetTickCount();
    TickType_t last_poll = xTaskGetTickCount();
    TickType_t last_alive = xTaskGetTickCount();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
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
        uint8_t links[MAX_TAPS * 4];
        int ln;
        bool send_links;
        lock();
        ln = build_links(links, sizeof(links));
        send_links = s_links_dirty || beat;
        s_links_dirty = false;
        unlock();
        if (send_links) {
            ESP_LOGI(TAG, "→ 연결 %d대", ln / 4);
            hub_uplink_send(HUB_RK_CMD_LINKS, links, ln);
            uint8_t pairs[MAX_PAIRS_BYTES];
            int pn = hub_pairs_blob(pairs, sizeof(pairs));
            hub_uplink_send(HUB_RK_CMD_PAIRS, pairs, pn);   /* 짝(없으면 0바이트 = 전부 단독/암묵) */
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

void hub_rinnai_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        xTaskCreate(zb_task, "hub_rk_zb", 3072, NULL, 3, &s_task);
    }
}
