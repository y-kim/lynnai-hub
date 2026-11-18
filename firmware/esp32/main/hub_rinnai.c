/*
 * hub_rinnai.c -- 중계 MQTT 엿보기(상태) + 명령 주입. 설명은 hub_rinnai.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "hub_rinnai.h"

static const char *TAG = "HUB_RINNAI";

#define CAP_DOWN    768             /* 기기→클라우드 패킷 하나 (f09 res ~300바이트) */
#define BLOB_MAX    64
#define CMD_MAX     320
#define HEARTBEAT_S 300             /* 바뀐 게 없어도 이 간격으로 다시 보낸다(드라이버가 끊김 판단) */

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
    uint8_t pkt[CMD_MAX];
} cmd_t;

struct hub_link_tap {
    framer_t down, up;
    uint8_t kind;                   /* 0 = 아직 모름 */
    char base[72];                  /* rinnai/RK/01/RK/f06/23/<MAC> */
    QueueHandle_t cmds;
    bool used;
};

#define MAX_TAPS 3
static hub_link_tap_t s_taps[MAX_TAPS];
static SemaphoreHandle_t s_lock;

/* 마지막 상태(종류별) — Zigbee 로 보낼 것 */
static struct {
    uint8_t blob[BLOB_MAX];
    int n;
    bool dirty;
} s_state[2];                       /* 0 = f06, 1 = f09 */
static uint8_t s_links_sent = 0xff;
static TaskHandle_t s_task;

static int kind_idx(uint8_t kind) { return kind == HUB_KIND_F06 ? 0 : kind == HUB_KIND_F09 ? 1 : -1; }

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void kick(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
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
static void on_down_packet(hub_link_tap_t *t, const uint8_t *p, int n, int h)
{
    if ((p[0] >> 4) != 3 || n < h + 2) {
        return;
    }
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
    if (!slash || (strcmp(slash, "/inf") && strcmp(slash, "/res"))) {
        return;
    }
    uint8_t kind = strstr(topic, "/f06/") ? HUB_KIND_F06 : strstr(topic, "/f09/") ? HUB_KIND_F09 : 0;
    if (!kind || slash - topic >= (int)sizeof(t->base)) {
        return;
    }
    if (t->kind != kind) {
        lock();
        t->kind = kind;
        memcpy(t->base, topic, slash - topic);
        t->base[slash - topic] = 0;
        unlock();
        ESP_LOGI(TAG, "링크 = f0%d (%s)", kind, t->base);
        kick();
    }

    /* "id":"70","data":"<hex>" */
    const char *key = "\"id\":\"70\",\"data\":\"";
    int kl = strlen(key), plen = n - off;
    const uint8_t *pl = p + off;
    for (int i = 0; i + kl < plen; i++) {
        if (memcmp(pl + i, key, kl)) {
            continue;
        }
        uint8_t blob[BLOB_MAX];
        int bn = 0;
        for (int j = i + kl; j + 1 < plen && isxdigit(pl[j]) && isxdigit(pl[j + 1]) && bn < BLOB_MAX; j += 2) {
            char hx[3] = { (char)pl[j], (char)pl[j + 1], 0 };
            blob[bn++] = (uint8_t)strtoul(hx, NULL, 16);
        }
        int k = kind_idx(kind);
        lock();
        if (bn && (s_state[k].n != bn || memcmp(s_state[k].blob, blob, bn))) {
            memcpy(s_state[k].blob, blob, bn);
            s_state[k].n = bn;
            s_state[k].dirty = true;
            unlock();
            ESP_LOGI(TAG, "f0%d 상태 바뀜 (%d바이트)", kind, bn);
            ESP_LOG_BUFFER_HEX(TAG, blob, bn);
            kick();
        } else {
            unlock();
        }
        return;
    }
}

/* ---------------------------------------------------------------- 링크 */

hub_link_tap_t *hub_rinnai_link_open(void)
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
        t->kind = 0;
        t->base[0] = 0;
        if (!t->cmds) {
            t->cmds = xQueueCreate(4, sizeof(cmd_t *));
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
    free(t->down.pkt);
    t->down.pkt = NULL;
    cmd_t *c;
    while (t->cmds && xQueueReceive(t->cmds, &c, 0) == pdTRUE) {
        free(c);
    }
    unlock();
    kick();
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
        framer_feed(t, &t->up, data, n, NULL);
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

static void command_one(const char *text, int len);

/* "f06 80=1;f06 b1=1" — 세미콜론으로 여러 개(각각 따로 /set 으로 보낸다) */
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

/* "f09 a0=7,b1=1" → 해당 링크 큐에 MQTT PUBLISH(QoS 0) */
static void command_one(const char *text, int len)
{
    char s[128];
    if (len <= 0 || len >= (int)sizeof(s)) {
        return;
    }
    memcpy(s, text, len);
    s[len] = 0;
    if (!strcmp(s, "refresh")) {
        lock();
        s_state[0].dirty = s_state[0].n > 0;
        s_state[1].dirty = s_state[1].n > 0;
        s_links_sent = 0xff;
        unlock();
        kick();
        return;
    }
    uint8_t kind = !strncmp(s, "f06 ", 4) ? HUB_KIND_F06 : !strncmp(s, "f09 ", 4) ? HUB_KIND_F09 : 0;
    if (!kind) {
        ESP_LOGW(TAG, "모르는 명령: %s", s);
        return;
    }

    /* 클라우드가 보내는 모양 그대로: "sum" = rpc 항목 수, 값은 소문자 16진(예 "1c") */
    for (char *c = s + 4; *c; c++) {
        *c = tolower((unsigned char)*c);
    }
    char rpc[192];
    int r = 0, items = 0;
    for (char *p = s + 4, *save = NULL, *tok = strtok_r(p, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq || !token_ok(tok, eq - tok, 2) || !token_ok(eq + 1, strlen(eq + 1), 16)) {
            ESP_LOGW(TAG, "잘못된 명령 항목: %s", tok);
            return;
        }
        *eq = 0;
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
    hub_link_tap_t *t = NULL;
    for (int i = 0; i < MAX_TAPS; i++) {
        if (s_taps[i].used && s_taps[i].kind == kind) {
            t = &s_taps[i];
        }
    }
    if (!t) {
        unlock();
        ESP_LOGW(TAG, "f0%d 연결 없음 — 명령 버림: %s", kind, s);
        return;
    }
    char topic[80];
    int tl = snprintf(topic, sizeof(topic), "%s/set", t->base);
    cmd_t *c = malloc(sizeof(cmd_t));
    int rl = 2 + tl + j;
    if (!c || rl > 16383 || 3 + rl > CMD_MAX) {
        unlock();
        free(c);
        return;
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
        unlock();
        ESP_LOGW(TAG, "명령 큐 가득 참");
        return;
    }
    unlock();
    ESP_LOGI(TAG, "명령 → %s %s", topic, json);
}

/* ---------------------------------------------------------------- Zigbee 로 내보내기 */

static void zb_task(void *arg)
{
    TickType_t last_beat = xTaskGetTickCount();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000));
        bool beat = xTaskGetTickCount() - last_beat >= pdMS_TO_TICKS(HEARTBEAT_S * 1000);
        if (beat) {
            last_beat = xTaskGetTickCount();
        }
        uint8_t mask = 0;
        uint8_t blob[2][BLOB_MAX];
        int n[2] = { 0, 0 };
        lock();
        for (int i = 0; i < MAX_TAPS; i++) {
            if (s_taps[i].used && s_taps[i].kind) {
                mask |= s_taps[i].kind == HUB_KIND_F06 ? 1 : 2;
            }
        }
        for (int k = 0; k < 2; k++) {
            if (s_state[k].n && (s_state[k].dirty || beat)) {
                memcpy(blob[k], s_state[k].blob, s_state[k].n);
                n[k] = s_state[k].n;
                s_state[k].dirty = false;
            }
        }
        bool links = mask != s_links_sent || beat;
        s_links_sent = mask;
        unlock();
        if (links) {
            hub_zigbee_send_links(mask);
        }
        for (int k = 0; k < 2; k++) {
            if (n[k]) {
                vTaskDelay(pdMS_TO_TICKS(200));
                hub_zigbee_send_state(k ? HUB_KIND_F09 : HUB_KIND_F06, blob[k], n[k]);
            }
        }
    }
}

void hub_rinnai_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        xTaskCreate(zb_task, "hub_rk_zb", 3072, NULL, 3, &s_task);
    }
}
