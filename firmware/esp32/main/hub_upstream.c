/*
 * hub_upstream.c -- "멀티 허브" 중심(host) 집계 + 이 허브 라벨 + 설정/관리화면. 설명은 hub_upstream.h.
 *
 * 위성(guest) 쪽 전송은 hub_lan.c 가 맡는다(전화 거는 쪽 = 다이얼러). 이 파일은:
 *   - 라벨/모드/중심IP/토큰 설정(NVS "label","upstream")
 *   - 중심(host): 위성별 테이블(연결 fd·id·라벨·IP·토큰칸) + 위성이 올린 f09 방 테이블(위성 index 태그)
 *                 → 한 Zigbee 로 병합(LINKS/MODELS/LABELS), STATE 흘려보내기, 명령 라우팅, 목록/삭제
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "hub_store.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "hub_upstream.h"
#include "hub_uplink.h"
#include "hub_lan.h"

static const char *TAG = "HUB_UP";

#define MAX_GUESTS   4
#define MAX_REMOTE   8           /* 위성 전체에서 끌어온 f09 조절기 수(방이 아니라 조절기 단위) */
#define TOKEN_LEN    32
#define LABEL_MAX    24          /* UTF-8 바이트 */

/* ---------------------------------------------------------------- 이 허브 라벨 */

static char s_label[LABEL_MAX + 1];

void hub_label_load(void)
{
    nvs_handle_t h;
    if (nvs_open("label", NVS_READWRITE, &h) == ESP_OK) {
        size_t n = sizeof(s_label);
        if (hs_get_str(h, "label", "s", s_label, &n) != ESP_OK) {
            s_label[0] = 0;
        }
        nvs_close(h);
    }
}

int hub_label_get(char *out, int cap)
{
    int n = (int)strlen(s_label);
    if (n >= cap) {
        n = cap - 1;
    }
    if (n < 0) {
        n = 0;
    }
    memcpy(out, s_label, n);
    out[n] = 0;
    return n;
}

static esp_err_t label_save(const char *s)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("label", NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    e = hs_set_str(h, "s", s);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e == ESP_OK) {
        snprintf(s_label, sizeof(s_label), "%s", s);
    }
    return e;
}

/* ---------------------------------------------------------------- 설정(NVS "upstream") */

static uint8_t s_mode;            /* HUB_UP_OFF/HOST/GUEST */
static char s_ip[40];             /* 위성: 중심 허브 IP */
static char s_tok[TOKEN_LEN + 1]; /* 위성: 중심에 제시할 토큰 */

static void cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open("upstream", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint8_t m = 0;
    hs_get_u8(h, "upstream", "mode", &m);
    s_mode = m <= HUB_UP_GUEST ? m : HUB_UP_OFF;
    size_t n = sizeof(s_ip);
    if (hs_get_str(h, "upstream", "ip", s_ip, &n) != ESP_OK) {
        s_ip[0] = 0;
    }
    n = sizeof(s_tok);
    if (hs_get_str(h, "upstream", "tok", s_tok, &n) != ESP_OK || strlen(s_tok) != TOKEN_LEN) {
        s_tok[0] = 0;
    }
    nvs_close(h);
}

/* 토큰이 없으면 한 번 만들어 저장한다(중심 허브가 페어링 창에서 받아 둘 값) */
static void ensure_token(void)
{
    if (strlen(s_tok) == TOKEN_LEN) {
        return;
    }
    for (int i = 0; i < TOKEN_LEN; i++) {
        s_tok[i] = "0123456789abcdef"[esp_random() & 0xf];
    }
    s_tok[TOKEN_LEN] = 0;
    nvs_handle_t h;
    if (nvs_open("upstream", NVS_READWRITE, &h) == ESP_OK) {
        if (hs_set_str(h, "tok", s_tok) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

static esp_err_t cfg_save(uint8_t mode, const char *ip)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("upstream", NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    if (e == ESP_OK) e = hs_set_u8(h, "mode", mode);
    if (e == ESP_OK) e = hs_set_str(h, "ip", ip);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) {
        s_mode = mode;
        snprintf(s_ip, sizeof(s_ip), "%s", ip);
    }
    return e;
}

/* ---------------------------------------------------------------- 중심(host) 테이블 */

typedef struct {
    bool used, online;
    int  fd;                      /* 이 위성으로 명령 보낼 소켓(-1 = 끊김) */
    int  tokslot;                 /* hub_lan 토큰 칸(삭제 때 폐기) */
    char id[16];
    char label[LABEL_MAX + 1];
    char ip[40];
} guest_t;

typedef struct {
    bool used, online;
    int  guest;                   /* 소속 위성 index */
    uint8_t mac3[3];
    uint8_t blob[HUB_BLOB_MAX];
    int n;
    int16_t model;
} remote_t;

static SemaphoreHandle_t s_lock;
static guest_t  s_guest[MAX_GUESTS];
static remote_t s_rem[MAX_REMOTE];

static void lk(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void uk(void) { if (s_lock) xSemaphoreGive(s_lock); }

static bool maceq(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 3); }

static remote_t *rem_find(const uint8_t *mac3)
{
    for (int i = 0; i < MAX_REMOTE; i++) {
        if (s_rem[i].used && maceq(s_rem[i].mac3, mac3)) {
            return &s_rem[i];
        }
    }
    return NULL;
}

static remote_t *rem_add(const uint8_t *mac3, int gi)
{
    for (int i = 0; i < MAX_REMOTE; i++) {
        if (!s_rem[i].used) {
            memset(&s_rem[i], 0, sizeof(s_rem[i]));
            s_rem[i].used = true;
            s_rem[i].guest = gi;
            memcpy(s_rem[i].mac3, mac3, 3);
            s_rem[i].model = -1;
            return &s_rem[i];
        }
    }
    return NULL;
}

/* 소속 위성의 라벨(없으면 길이 0) */
static int guest_label(int gi, char *out, int cap)
{
    int n = 0;
    if (gi >= 0 && gi < MAX_GUESTS && s_guest[gi].used) {
        n = (int)strlen(s_guest[gi].label);
        if (n >= cap) n = cap - 1;
        memcpy(out, s_guest[gi].label, n);
    }
    out[n] = 0;
    return n;
}

/* ---------------------------------------------------------------- 공개: 설정 getter */

bool hub_upstream_enabled(void) { return s_mode != HUB_UP_OFF; }
int  hub_upstream_mode(void)    { return s_mode; }

int hub_upstream_guest_count(int *online)   /* 중심: 등록된 위성 수(온라인 수는 *online) */
{
    int total = 0, on = 0;
    lk();
    for (int i = 0; i < MAX_GUESTS; i++) {
        if (s_guest[i].used) {
            total++;
            if (s_guest[i].online) on++;
        }
    }
    uk();
    if (online) *online = on;
    return total;
}

bool hub_upstream_guest_target(char *ip, int ipcap, char *tok, int tokcap)
{
    if (s_mode != HUB_UP_GUEST) {
        return false;
    }
    snprintf(ip, ipcap, "%s", s_ip);
    snprintf(tok, tokcap, "%s", s_tok);
    return ip[0] && strlen(tok) == TOKEN_LEN;
}

/* ---------------------------------------------------------------- 중심: 위성 연결 */

void hub_rinnai_resend(void);   /* 병합 목록 다시 보내게 깨운다(hub_rinnai.c) */

int hub_upstream_guest_join(int fd, int tokslot, const char *id, const char *label, const char *ip)
{
    lk();
    int gi = -1;
    for (int i = 0; i < MAX_GUESTS; i++) {           /* 같은 id 재접속이면 그 칸 */
        if (s_guest[i].used && id && id[0] && !strcmp(s_guest[i].id, id)) {
            gi = i;
            break;
        }
    }
    if (gi < 0) {
        for (int i = 0; i < MAX_GUESTS; i++) {
            if (!s_guest[i].used) {
                gi = i;
                memset(&s_guest[i], 0, sizeof(s_guest[i]));
                break;
            }
        }
    }
    int oldfd = -1;
    if (gi >= 0) {
        if (s_guest[gi].used && s_guest[gi].fd >= 0 && s_guest[gi].fd != fd) {
            oldfd = s_guest[gi].fd;                   /* 같은 위성의 옛 연결 — 즉시 정리 */
        }
        s_guest[gi].used = true;
        s_guest[gi].online = true;
        s_guest[gi].fd = fd;
        s_guest[gi].tokslot = tokslot;
        snprintf(s_guest[gi].id, sizeof(s_guest[gi].id), "%s", id ? id : "");
        if (label && label[0]) {
            snprintf(s_guest[gi].label, sizeof(s_guest[gi].label), "%s", label);
        }
        snprintf(s_guest[gi].ip, sizeof(s_guest[gi].ip), "%s", ip ? ip : "");
    }
    uk();
    if (oldfd >= 0) {
        shutdown(oldfd, SHUT_RDWR);                   /* 옛 연결의 태스크가 빨리 빠져나오게(fd 다르니 online 은 안 건드림) */
    }
    if (gi >= 0) {
        ESP_LOGI(TAG, "위성 허브 연결: %s (%s) 칸 %d", (label && label[0]) ? label : "?", ip ? ip : "?", gi);
    }
    return gi;
}

void hub_upstream_guest_leave(int gi, int fd)
{
    if (gi < 0 || gi >= MAX_GUESTS) {
        return;
    }
    bool any = false, mine = false;
    lk();
    if (s_guest[gi].used && s_guest[gi].fd == fd) {   /* 떠나는 연결이 '현재' 연결일 때만 — 재접속이 이어받았으면 건드리지 않는다 */
        mine = true;
        s_guest[gi].online = false;
        s_guest[gi].fd = -1;
        for (int i = 0; i < MAX_REMOTE; i++) {        /* 그 위성 방들을 오프라인 */
            if (s_rem[i].used && s_rem[i].guest == gi && s_rem[i].online) {
                s_rem[i].online = false;
                any = true;
            }
        }
    }
    uk();
    if (any) {
        hub_rinnai_resend();
    }
    if (mine) {
        ESP_LOGW(TAG, "위성 허브 연결 끊김 (칸 %d)", gi);
    }
}

/* 위성이 올린 프레임(f09 만) */
void hub_upstream_guest_frame(int gi, uint8_t cmd, const uint8_t *p, int n)
{
    if (gi < 0 || gi >= MAX_GUESTS) {
        return;
    }
    if (cmd == HUB_RK_CMD_STATE && n >= 4 && p[0] == HUB_KIND_F09) {
        lk();
        remote_t *r = rem_find(p + 1);
        bool isnew = !r;
        if (!r) {
            r = rem_add(p + 1, gi);
        }
        if (r) {
            r->guest = gi;
            r->online = true;
            r->n = n - 4;
            if (r->n > HUB_BLOB_MAX) {
                r->n = HUB_BLOB_MAX;
            }
            memcpy(r->blob, p + 4, r->n);
        }
        uk();
        hub_uplink_send(HUB_RK_CMD_STATE, p, n);     /* ST 로 그대로(MAC 이 다르니 안전) */
        if (isnew) {
            hub_rinnai_resend();
        }
    } else if (cmd == HUB_RK_CMD_LINKS) {
        bool changed = false;
        lk();
        uint8_t seen[MAX_REMOTE][3];
        int ns = 0;
        for (int i = 0; i + 4 <= n; i += 4) {
            if (p[i] != HUB_KIND_F09) {
                continue;                            /* 보일러 등은 위성에서 안 받음 */
            }
            const uint8_t *mac = p + i + 1;
            if (ns < MAX_REMOTE) {
                memcpy(seen[ns++], mac, 3);
            }
            remote_t *r = rem_find(mac);
            if (!r) {
                r = rem_add(mac, gi);
                changed = true;
            }
            if (r) {
                r->guest = gi;
                if (!r->online) {
                    r->online = true;
                    changed = true;
                }
            }
        }
        for (int i = 0; i < MAX_REMOTE; i++) {        /* 이 위성 것 중 목록에 없는 건 오프라인 */
            if (!s_rem[i].used || s_rem[i].guest != gi || !s_rem[i].online) {
                continue;
            }
            bool here = false;
            for (int j = 0; j < ns; j++) {
                if (maceq(s_rem[i].mac3, seen[j])) {
                    here = true;
                    break;
                }
            }
            if (!here) {
                s_rem[i].online = false;
                changed = true;
            }
        }
        uk();
        if (changed) {
            hub_rinnai_resend();
        }
    } else if (cmd == HUB_RK_CMD_MODELS) {
        lk();
        for (int i = 0; i + 5 <= n; i += 5) {
            if (p[i] != HUB_KIND_F09) {
                continue;
            }
            remote_t *r = rem_find(p + i + 1);
            if (r && r->guest == gi) {
                r->model = p[i + 4];
            }
        }
        uk();
    } else if (cmd == HUB_RK_CMD_LABELS) {
        lk();
        for (int i = 0; i + 5 <= n;) {
            uint8_t kind = p[i];
            int ln = p[i + 4];
            if (i + 5 + ln > n) {
                break;
            }
            if (kind == HUB_KIND_F09 && ln > 0) {     /* 위성이 자기 방에 붙인 라벨 = 위성 라벨 */
                int c = ln < LABEL_MAX ? ln : LABEL_MAX;
                memcpy(s_guest[gi].label, p + i + 5, c);
                s_guest[gi].label[c] = 0;
            }
            i += 5 + ln;
        }
        uk();
    } else if (cmd == HUB_RK_CMD_FORGET && n >= 4 && p[0] == HUB_KIND_F09) {
        lk();
        remote_t *r = rem_find(p + 1);
        if (r && r->guest == gi) {
            r->used = r->online = false;
        }
        uk();
        hub_uplink_send(HUB_RK_CMD_FORGET, p, n);
        hub_rinnai_resend();
    }
    /* 그 밖(GAS/PLUGINS/MODE/BOILCFG/TARIFF/ROOM/ALIVE/EXPOSE/PAIRS/보일러 STATE)은 버린다 — "위성은 방만" */
}

/* ---------------------------------------------------------------- 병합 helper (hub_rinnai push) */

int hub_upstream_links(uint8_t *out, int cap)
{
    int n = 0;
    lk();
    for (int i = 0; i < MAX_REMOTE && n + 4 <= cap; i++) {
        if (s_rem[i].used && s_rem[i].online) {
            out[n++] = HUB_KIND_F09;
            memcpy(out + n, s_rem[i].mac3, 3);
            n += 3;
        }
    }
    uk();
    return n;
}

int hub_upstream_models(uint8_t *out, int cap)
{
    int n = 0;
    lk();
    for (int i = 0; i < MAX_REMOTE && n + 5 <= cap; i++) {
        if (s_rem[i].used && s_rem[i].online && s_rem[i].model >= 0) {
            out[n++] = HUB_KIND_F09;
            memcpy(out + n, s_rem[i].mac3, 3);
            n += 3;
            out[n++] = (uint8_t)s_rem[i].model;
        }
    }
    uk();
    return n;
}

int hub_upstream_labels(uint8_t *out, int cap)
{
    int n = 0;
    lk();
    for (int i = 0; i < MAX_REMOTE; i++) {
        if (!s_rem[i].used || !s_rem[i].online) {
            continue;
        }
        char lb[LABEL_MAX + 1];
        int ll = guest_label(s_rem[i].guest, lb, sizeof(lb));
        if (ll <= 0 || n + 5 + ll > cap) {
            continue;
        }
        out[n++] = HUB_KIND_F09;
        memcpy(out + n, s_rem[i].mac3, 3);
        n += 3;
        out[n++] = (uint8_t)ll;
        memcpy(out + n, lb, ll);
        n += ll;
    }
    uk();
    return n;
}

void hub_upstream_resend(void)
{
    uint8_t msg[4 + HUB_BLOB_MAX];
    for (int i = 0; i < MAX_REMOTE; i++) {
        int bn = 0;
        lk();
        if (s_rem[i].used && s_rem[i].online && s_rem[i].n) {
            msg[0] = HUB_KIND_F09;
            memcpy(msg + 1, s_rem[i].mac3, 3);
            bn = s_rem[i].n;
            memcpy(msg + 4, s_rem[i].blob, bn);
        }
        uk();
        if (bn) {
            hub_uplink_send(HUB_RK_CMD_STATE, msg, bn + 4);
        }
    }
}

bool hub_upstream_is_remote(uint8_t kind, const uint8_t *mac3)
{
    if (kind != HUB_KIND_F09) {
        return false;
    }
    lk();
    remote_t *r = rem_find(mac3);
    bool yes = r && r->online;
    uk();
    return yes;
}

/* "<cmd 2hex> <payload hex>\n" 한 줄을 fd 로 보낸다 */
static void send_request(int fd, const char *text, int len)
{
    char line[8 + 2 * 120];
    int k = snprintf(line, sizeof(line), "%02x ", HUB_RK_CMD_REQUEST);
    for (int i = 0; i < len && k + 2 < (int)sizeof(line); i++) {
        k += snprintf(line + k, sizeof(line) - k, "%02x", (uint8_t)text[i]);
    }
    line[k++] = '\n';
    if (send(fd, line, k, MSG_DONTWAIT) != k) {
        shutdown(fd, SHUT_RDWR);                      /* 못 보내면 끊어 재접속 유도 */
    }
}

void hub_upstream_forward(const uint8_t *mac3, const char *text, int len)
{
    if (len <= 0 || len > 120) {
        return;
    }
    lk();
    if (!mac3) {                                      /* refresh 등: 모든 온라인 위성에 방송 */
        for (int i = 0; i < MAX_GUESTS; i++) {
            if (s_guest[i].used && s_guest[i].online && s_guest[i].fd >= 0) {
                send_request(s_guest[i].fd, text, len);
            }
        }
    } else {
        remote_t *r = rem_find(mac3);
        if (r && r->online && r->guest >= 0 && r->guest < MAX_GUESTS) {
            guest_t *g = &s_guest[r->guest];
            if (g->used && g->online && g->fd >= 0) {
                send_request(g->fd, text, len);
            }
        }
    }
    uk();
}

int hub_upstream_snapshot(hub_dev_t *out, int max)
{
    int n = 0;
    lk();
    for (int i = 0; i < MAX_REMOTE && n < max; i++) {
        if (!s_rem[i].used || !s_rem[i].n) {
            continue;
        }
        memset(&out[n], 0, sizeof(out[n]));
        out[n].kind = HUB_KIND_F09;
        memcpy(out[n].mac3, s_rem[i].mac3, 3);
        out[n].n = s_rem[i].n;
        memcpy(out[n].blob, s_rem[i].blob, s_rem[i].n);
        out[n].linked = s_rem[i].online;
        out[n].model = s_rem[i].model;
        n++;
    }
    uk();
    return n;
}

int hub_upstream_label_of(uint8_t kind, const uint8_t *mac3, char *out, int cap)
{
    if (kind == HUB_KIND_F09) {
        lk();
        remote_t *r = rem_find(mac3);
        if (r) {
            int n = guest_label(r->guest, out, cap);
            uk();
            return n;
        }
        uk();
    }
    return hub_label_get(out, cap);   /* 로컬 기기 = 이 허브 라벨 */
}

/* ---------------------------------------------------------------- 시작 */

void hub_upstream_start(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_GUESTS; i++) {
        s_guest[i].fd = -1;
    }
    cfg_load();
    ensure_token();
    /* 위성 전송은 hub_lan.c 의 다이얼러가, 중심 수신은 hub_lan.c 의 인바운드가 담당한다 */
}

/* ---------------------------------------------------------------- 관리 화면 */

static int hexv(char c) { return isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10); }

static void url_dec(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            *o++ = (char)(hexv(s[1]) * 16 + hexv(s[2]));
            s += 2;
        } else if (*s == '+') {
            *o++ = ' ';
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void cat_html(char *o, size_t cap, const char *s)   /* 아주 작은 이스케이프 */
{
    char t[128];
    int k = 0;
    for (; *s && k < (int)sizeof(t) - 6; s++) {
        if (*s == '<') { memcpy(t + k, "&lt;", 4); k += 4; }
        else if (*s == '>') { memcpy(t + k, "&gt;", 4); k += 4; }
        else if (*s == '&') { memcpy(t + k, "&amp;", 5); k += 5; }
        else if (*s == '"') { memcpy(t + k, "&quot;", 6); k += 6; }
        else { t[k++] = *s; }
    }
    t[k] = 0;
    strlcat(o, t, cap);
}

static void rad(char *o, size_t cap, const char *v, const char *txt, bool on)
{
    strlcat(o, "<label class=chk><input type=radio name=mode value=", cap);
    strlcat(o, v, cap);
    if (on) strlcat(o, " checked", cap);
    strlcat(o, "> ", cap);
    strlcat(o, txt, cap);
    strlcat(o, "</label>", cap);
}

void hub_upstream_render(char *o, size_t cap)
{
    char lbl[LABEL_MAX + 1];
    hub_label_get(lbl, sizeof(lbl));

    strlcat(o, "<h2>멀티 허브</h2><div class=card>", cap);
    strlcat(o, "<p class=muted>여러 lynnai 허브를 하나로 묶습니다. <b>중심 허브</b> 하나가 <b>위성 허브</b>들의 "
               "방을 모아 SmartThings/HA 에 한 번에 보여 줍니다. 라벨은 방 이름 앞에 붙어 층을 구분합니다.</p>", cap);
    strlcat(o, "<p class=warn>이 모드에서는 <b>모든 허브가 고정 IP</b>를 써야 합니다 — 공유기에서 각 허브에 "
               "DHCP 예약(고정 IP)을 설정하세요.</p>", cap);

    strlcat(o, "<form id=mhform method=post action=/admin/upstream>", cap);
    strlcat(o, "<label>모드</label>", cap);
    rad(o, cap, "0", "끔", s_mode == HUB_UP_OFF);
    rad(o, cap, "1", "중심 허브 (여기로 모음)", s_mode == HUB_UP_HOST);
    rad(o, cap, "2", "위성 허브 (중심에 올려보냄)", s_mode == HUB_UP_GUEST);
    strlcat(o, "<div id=mhlabel><label>허브 라벨</label><input name=label maxlength=24 value=\"", cap);
    cat_html(o, cap, lbl);
    strlcat(o, "\" placeholder=\"예: 2층\"></div>", cap);
    strlcat(o, "<div id=mhip><label>중심 허브 IP</label><input name=ip value=\"", cap);
    cat_html(o, cap, s_ip);
    strlcat(o, "\" placeholder=\"예: 192.168.0.23\"></div>", cap);
    strlcat(o, "<button class=sv>저장</button></form>", cap);
    /* 모드를 먼저 고르면 그 자리에서: 라벨은 켰을 때만, 중심 IP 는 위성일 때만 보이게 */
    strlcat(o, "<script>(function(){var F=document.getElementById('mhform');if(!F)return;"
               "function f(){var c=F.querySelector('input[name=mode]:checked');"
               "var m=c?c.value:'0';var L=document.getElementById('mhlabel'),P=document.getElementById('mhip');"
               "if(L)L.hidden=(m=='0');if(P)P.hidden=(m!='2');}"
               "F.querySelectorAll('input[name=mode]').forEach(function(e){e.addEventListener('change',f)});f();})();</script>", cap);

    if (s_mode == HUB_UP_HOST) {
        strlcat(o, "<hr><p class=muted>위성 허브를 붙이려면 아래를 누른 뒤 3분 안에 그 위성에서 "
                   "<b>중심 허브에 연결하기</b>를 누르게 하세요.</p>"
                   "<form method=post action=/admin/upstream data-toast>"
                   "<button name=allowpair value=1 class=lnk>다른 허브 연결 허용 (3분)</button></form>", cap);
        strlcat(o, "<p class=muted>연결된 위성 허브</p>", cap);
        bool any = false;
        lk();
        for (int i = 0; i < MAX_GUESTS; i++) {
            if (!s_guest[i].used) {
                continue;
            }
            any = true;
            strlcat(o, "<div class=row><span>", cap);
            cat_html(o, cap, s_guest[i].label[0] ? s_guest[i].label : s_guest[i].id);
            strlcat(o, " — ", cap);
            cat_html(o, cap, s_guest[i].ip[0] ? s_guest[i].ip : "?");
            strlcat(o, s_guest[i].online ? " <b class=ok>연결됨</b>" : " <span class=muted>끊김</span>", cap);
            strlcat(o, "</span><form method=post action=/admin/upstream data-toast><input type=hidden name=del value=\"", cap);
            cat_html(o, cap, s_guest[i].id);
            strlcat(o, "\"><button class=lnk>삭제</button></form></div>", cap);
        }
        uk();
        if (!any) {
            strlcat(o, "<p class=muted>— 아직 없음 —</p>", cap);
        }
    } else if (s_mode == HUB_UP_GUEST) {
        char st[160];
        snprintf(st, sizeof(st), "<hr><p class=muted>중심 허브(%s) 연결 상태: <b class=%s>%s</b></p>",
                 s_ip[0] ? s_ip : "미설정", hub_lan_guest_online() ? "ok" : "muted", hub_lan_guest_status());
        strlcat(o, st, cap);
        strlcat(o, "<form method=post action=/admin/upstream data-toast>"
                   "<button name=conn value=1 class=lnk>중심 허브에 연결하기</button></form>", cap);
    }
    strlcat(o, "</div>", cap);
}

/* 위성 삭제: 연결 끊고 토큰 폐기하고 그 방들 제거 */
static void guest_delete(const char *id)
{
    int fd = -1, slot = -1;
    lk();
    for (int i = 0; i < MAX_GUESTS; i++) {
        if (s_guest[i].used && !strcmp(s_guest[i].id, id)) {
            fd = s_guest[i].fd;
            slot = s_guest[i].tokslot;
            for (int j = 0; j < MAX_REMOTE; j++) {
                if (s_rem[j].used && s_rem[j].guest == i) {
                    s_rem[j].used = s_rem[j].online = false;
                }
            }
            memset(&s_guest[i], 0, sizeof(s_guest[i]));
            s_guest[i].fd = -1;
            break;
        }
    }
    uk();
    if (slot >= 0) {
        hub_lan_token_remove(slot);                   /* 다시 못 붙게 토큰 폐기 */
    }
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);                       /* 현재 연결 끊기 */
    }
    hub_rinnai_resend();
}

bool hub_upstream_apply(const char *form, char *msg, size_t len)
{
    char v[64];
    if (httpd_query_key_value(form, "allowpair", v, sizeof(v)) == ESP_OK) {
        hub_lan_pairing_open(180);
        snprintf(msg, len, "위성 허브 연결을 3분 동안 허용합니다 — 그 위성에서 지금 '중심 허브에 연결하기'를 누르세요");
        return true;
    }
    if (httpd_query_key_value(form, "del", v, sizeof(v)) == ESP_OK) {
        url_dec(v);
        guest_delete(v);
        snprintf(msg, len, "위성 허브를 삭제했습니다");
        return true;
    }
    if (httpd_query_key_value(form, "conn", v, sizeof(v)) == ESP_OK) {
        hub_lan_guest_repair();
        snprintf(msg, len, "중심 허브에 다시 연결합니다");
        return true;
    }

    /* URL 인코딩된 값은 한 바이트가 %XX 세 글자 — 받는 칸을 3배로(작으면 httpd_query_key_value 가 아무것도 안 써서
     * 한글 3자 이상 라벨이 빈 값으로 저장됐다). 풀고 나서 LABEL_MAX 바이트로 자르되 UTF-8 글자 중간에서 끊지 않는다 */
    char label[LABEL_MAX * 3 + 1] = "", ip[40] = "", mode[8] = "0";
    if (httpd_query_key_value(form, "label", label, sizeof(label)) == ESP_OK) {
        url_dec(label);
        size_t n = strlen(label);
        if (n > LABEL_MAX) {
            n = LABEL_MAX;
            while (n > 0 && ((unsigned char)label[n] & 0xC0) == 0x80) n--;   /* 이어지는 바이트면 글자 앞까지 */
            label[n] = 0;
        }
    }
    if (httpd_query_key_value(form, "ip", ip, sizeof(ip)) == ESP_OK) {
        url_dec(ip);
    }
    if (httpd_query_key_value(form, "mode", mode, sizeof(mode)) == ESP_OK) {
        url_dec(mode);
    }
    uint8_t m = (uint8_t)atoi(mode);
    if (m > HUB_UP_GUEST) {
        m = HUB_UP_OFF;
    }

    esp_err_t e1 = label_save(label);
    esp_err_t e2 = cfg_save(m, ip);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        snprintf(msg, len, "저장 실패");
        return false;
    }
    if (m == HUB_UP_GUEST) {
        ensure_token();
    }
    hub_lan_guest_reconnect();                /* 모드/IP 반영해 위성 다이얼러 다시 */
    hub_rinnai_resend();                       /* 라벨 바뀜 → 다시 보냄 */
    snprintf(msg, len, "저장했습니다 (라벨 \"%s\" · %s)", label,
             m == HUB_UP_HOST ? "중심 허브" : m == HUB_UP_GUEST ? "위성 허브" : "끔");
    return true;
}
