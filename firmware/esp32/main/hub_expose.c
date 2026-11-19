/*
 * hub_expose.c -- 노출 통로. 설명·비트·전송 형식은 hub_expose.h.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "hub_store.h"

#include "hub_expose.h"
#include "hub_uplink.h"
#include "hub_rinnai.h"

static const char *TAG = "HUB_EXPOSE";

/* 키 '3': 노출 방식이 기기별+종류별로 바뀌어 옛 마스크(zb2/lan2/mt2)는 뜻이 달라 무시 → 전부 보임으로 시작 */
static const char *const CH_KEY[EXPOSE_NCH] = { "zb3", "lan3", "mt3" };
static const char *const HID_KEY[EXPOSE_NCH] = { "hzb", "hlan", "hmt" };
static const char *const CH_NAME[EXPOSE_NCH] = { "Zigbee", "LAN", "Matter" };
static const uint8_t CH_UPLINK[EXPOSE_NCH] = { HUB_UPLINK_ZIGBEE, HUB_UPLINK_LAN, HUB_UPLINK_MATTER };

#define ALL_BITS 0x7FFFFFFFu
#define MAX_HID  8                      /* 기기 수(hub_rinnai MAX_DEV 6) + 여유 */

typedef struct {
    uint8_t kind;
    uint8_t mac[3];
    uint8_t hid;                        /* bit0 기기 자신, bit1~7 방 z1~z7 */
} hid_t;

static uint32_t s_mask[EXPOSE_NCH] = { ALL_BITS, ALL_BITS, ALL_BITS };
static hid_t s_hid[EXPOSE_NCH][MAX_HID];
static int s_nhid[EXPOSE_NCH];

const hub_room_t HUB_ROOMS[HUB_NROOMS] = {
    { "방1", 0, 1 }, { "방2", 7, 2 }, { "방3", 14, 4 }, { "방4", 21, 5 },
    { "방5", 28, 6 }, { "방6", 35, 7 }, { "거실", 42, 3 },
};

bool hub_room_present(const hub_dev_t *d, int i)
{
    int s = HUB_ROOMS[i].stat;
    return d->kind == HUB_KIND_F09 && s + 3 < d->n && (d->blob[s] || d->blob[s + 3]);
}

/* 종류별 컨트롤 표 */
typedef struct {
    int bit;
    const char *label;
} ctl_t;
static const ctl_t BOILER_CTL[] = {
    { 1, "전원" }, { 2, "난방 모드 (난방/외출/끄기)" }, { 3, "난방 방식 (온돌/실내)" }, { 4, "난방 온도" },
    { 5, "온수 켜기/끄기" }, { 6, "온수 온도" }, { 7, "온수 예열" },
};
static const ctl_t ROOM_CTL[] = { { 8, "켜기/끄기" }, { 9, "설정 온도" }, { 10, "외출" } };
#define NB (int)(sizeof(BOILER_CTL) / sizeof(BOILER_CTL[0]))
#define NR (int)(sizeof(ROOM_CTL) / sizeof(ROOM_CTL[0]))

/* ---------------------------------------------------------------- 저장 */

void hub_expose_load(void)
{
    nvs_handle_t h;
    if (nvs_open("expose", NVS_READWRITE, &h) == ESP_OK) {
        for (int c = 0; c < EXPOSE_NCH; c++) {
            hs_get_u32(h, "expose", CH_KEY[c], &s_mask[c]);
            size_t n = sizeof(s_hid[c]);
            if (hs_get_blob(h, "expose", HID_KEY[c], s_hid[c], &n) == ESP_OK && n % sizeof(hid_t) == 0) {
                s_nhid[c] = (int)(n / sizeof(hid_t));
            }
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "노출: Zigbee %08lx(숨김 %d), LAN %08lx(숨김 %d)", (unsigned long)s_mask[0], s_nhid[0],
             (unsigned long)s_mask[1], s_nhid[1]);
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("expose", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    for (int c = 0; c < EXPOSE_NCH; c++) {
        hs_set_u32(h, CH_KEY[c], s_mask[c]);
        if (s_nhid[c]) {
            hs_set_blob(h, HID_KEY[c], s_hid[c], s_nhid[c] * sizeof(hid_t));
        } else {
            hs_erase(h, HID_KEY[c]);
        }
    }
    nvs_commit(h);
    nvs_close(h);
}

static hid_t *hid_find(int c, uint8_t kind, const uint8_t mac3[3])
{
    for (int i = 0; i < s_nhid[c]; i++) {
        if (s_hid[c][i].kind == kind && !memcmp(s_hid[c][i].mac, mac3, 3)) {
            return &s_hid[c][i];
        }
    }
    return NULL;
}

static uint8_t hid_get(int c, uint8_t kind, const uint8_t mac3[3])
{
    hid_t *e = hid_find(c, kind, mac3);
    return e ? e->hid : 0;
}

/* 숨김 비트를 정한다(0 이 되면 항목을 빼서 목록을 짧게) */
static void hid_set(int c, uint8_t kind, const uint8_t mac3[3], uint8_t hid)
{
    hid_t *e = hid_find(c, kind, mac3);
    if (!e) {
        if (!hid || s_nhid[c] >= MAX_HID) {
            return;
        }
        e = &s_hid[c][s_nhid[c]++];
        e->kind = kind;
        memcpy(e->mac, mac3, 3);
    }
    e->hid = hid;
    if (!hid) {
        *e = s_hid[c][--s_nhid[c]];
    }
}

void hub_expose_forget(uint8_t kind, const uint8_t mac3[3])
{
    for (int c = 0; c < EXPOSE_NCH; c++) {
        hid_set(c, kind, mac3, 0);
    }
    save();
}

void hub_expose_send_all(void)
{
    for (int c = 0; c < EXPOSE_NCH; c++) {
        uint8_t buf[4 + MAX_HID * 5];
        memcpy(buf, &s_mask[c], 4);                           /* 리틀엔디언 u32 */
        int n = 4;
        for (int i = 0; i < s_nhid[c]; i++) {
            buf[n++] = s_hid[c][i].kind;
            memcpy(buf + n, s_hid[c][i].mac, 3);
            n += 3;
            buf[n++] = s_hid[c][i].hid;
        }
        hub_uplink_send_to(CH_UPLINK[c], HUB_RK_CMD_EXPOSE, buf, (uint16_t)n);
    }
}

/* ---------------------------------------------------------------- 화면 */

/* 사용하기로 한 통로(hub_uplink_mask)의 체크박스만. name 은 접두사 + "_<통로>" */
static void boxes(char *o, size_t cap, const char *name, bool (*on)(int c, const void *ctx), const void *ctx)
{
    char t[160];
    uint8_t active = hub_uplink_mask();
    for (int c = 0; c < EXPOSE_NCH; c++) {
        if (!(active & CH_UPLINK[c])) {
            continue;
        }
        snprintf(t, sizeof(t), "<label class=chk><input type=checkbox name=%s_%d%s> %s</label>", name, c,
                 on(c, ctx) ? " checked" : "", CH_NAME[c]);
        strlcat(o, t, cap);
    }
}

static bool bit_on(int c, const void *ctx) { return (s_mask[c] >> *(const int *)ctx) & 1; }

typedef struct {
    uint8_t kind;
    const uint8_t *mac;
    int sub;
} dev_ctx_t;
static bool dev_on(int c, const void *ctx)
{
    const dev_ctx_t *d = ctx;
    return !((hid_get(c, d->kind, d->mac) >> d->sub) & 1);
}

static void ctl_rows(char *o, size_t cap, const ctl_t *list, int n)
{
    char t[160], name[12];
    for (int i = 0; i < n; i++) {
        snprintf(t, sizeof(t), "<div class=row><span>%s</span><span>", list[i].label);
        strlcat(o, t, cap);
        snprintf(name, sizeof(name), "b%d", list[i].bit);
        boxes(o, cap, name, bit_on, &list[i].bit);
        strlcat(o, "</span></div>", cap);
    }
}

void hub_expose_render_types(char *o, size_t cap)
{
    strlcat(o, "<h2>노출 설정</h2><div class=card><p class=muted>"
               "기기 종류마다 플랫폼(SmartThings 등)에 보일 컨트롤을 고릅니다. 같은 종류의 모든 기기에 적용됩니다. "
               "기기를 통째로 숨기는 건 위 '연결된 기기'에서 합니다. 끄면 숨김(허브는 계속 중계·제어).</p>"
               "<form method=post action=/admin/expose>"
               "<div class=grpbox><h3 class=grp>🔥 보일러 조절기</h3><p class='muted sub'>보일러 기기의 컨트롤</p>", cap);
    ctl_rows(o, cap, BOILER_CTL, NB);
    strlcat(o, "</div><div class=grpbox><h3 class=grp>🌡️ 온도 조절기 (각 방)</h3>"
               "<p class='muted sub'>방마다 생기는 기기의 컨트롤 — 모든 방에 같이 적용</p>", cap);
    ctl_rows(o, cap, ROOM_CTL, NR);
    strlcat(o, "</div><button class=sv>저장</button></form></div>", cap);
}

void hub_expose_render_dev(char *o, size_t cap, uint8_t kind, const uint8_t mac3[3], int sub)
{
    char name[24];
    snprintf(name, sizeof(name), "h%d_%02x%02x%02x_%d", kind, mac3[0], mac3[1], mac3[2], sub);
    dev_ctx_t d = { kind, mac3, sub };
    boxes(o, cap, name, dev_on, &d);
}

/* 화면에 그린 기기·방을 폼에 적어 둔다 → 저장 때 그 사이 새로 붙은 기기를 '체크 안 됨=숨김'으로 오해하지 않게 */
void hub_expose_render_dev_marker(char *o, size_t cap, uint8_t kind, const uint8_t mac3[3], uint8_t subs)
{
    char t[64];
    snprintf(t, sizeof(t), "<input type=hidden name=x%d_%02x%02x%02x value=%u>", kind, mac3[0], mac3[1], mac3[2], subs);
    strlcat(o, t, cap);
}

void hub_expose_render_bit(char *o, size_t cap, int bit)
{
    char name[8];
    snprintf(name, sizeof(name), "b%d", bit);
    boxes(o, cap, name, bit_on, &bit);
}

/* ---------------------------------------------------------------- 적용 */

static bool checked(const char *form, const char *name, int c)
{
    char key[32], v[8];
    snprintf(key, sizeof(key), "%s_%d", name, c);
    return httpd_query_key_value(form, key, v, sizeof(v)) == ESP_OK;   /* 체크된 것만 온다 */
}

/* 화면에 있던 비트들만 폼대로 바꾼다(안 쓰는 통로·화면 밖 비트는 그대로) */
static void apply_bits(const char *form, uint32_t scope)
{
    uint8_t active = hub_uplink_mask();
    char name[8];
    for (int c = 0; c < EXPOSE_NCH; c++) {
        if (!(active & CH_UPLINK[c])) {
            continue;
        }
        for (int b = 0; b < 32; b++) {
            if ((scope >> b) & 1) {
                snprintf(name, sizeof(name), "b%d", b);
                s_mask[c] = checked(form, name, c) ? s_mask[c] | (1u << b) : s_mask[c] & ~(1u << b);
            }
        }
    }
}

bool hub_expose_apply(const char *form, char *err, size_t errlen)
{
    uint32_t scope = 0;
    for (int i = 0; i < NB; i++) {
        scope |= 1u << BOILER_CTL[i].bit;
    }
    for (int i = 0; i < NR; i++) {
        scope |= 1u << ROOM_CTL[i].bit;
    }
    apply_bits(form, scope);
    save();
    ESP_LOGI(TAG, "컨트롤 노출 바뀜: Zigbee %08lx, LAN %08lx", (unsigned long)s_mask[0], (unsigned long)s_mask[1]);
    hub_expose_send_all();
    return true;
}

void hub_expose_apply_bit(const char *form, int bit)
{
    if (bit != EXPOSE_ROOM_DEV && bit != EXPOSE_GAS_DEV) {
        return;
    }
    apply_bits(form, 1u << bit);
    save();
    hub_expose_send_all();
}

void hub_expose_apply_devices(const char *form)
{
    hub_dev_t dev[8];
    int nd = hub_rinnai_snapshot(dev, 8);
    uint8_t active = hub_uplink_mask();
    for (int i = 0; i < nd; i++) {
        char key[16], v[8], name[24];
        snprintf(key, sizeof(key), "x%d_%02x%02x%02x", dev[i].kind, dev[i].mac3[0], dev[i].mac3[1], dev[i].mac3[2]);
        if (httpd_query_key_value(form, key, v, sizeof(v)) != ESP_OK) {
            continue;                                   /* 화면에 없던 기기: 그대로 */
        }
        uint8_t subs = (uint8_t)atoi(v);
        for (int c = 0; c < EXPOSE_NCH; c++) {
            if (!(active & CH_UPLINK[c])) {
                continue;
            }
            uint8_t hid = hid_get(c, dev[i].kind, dev[i].mac3);
            for (int sub = 0; sub < 8; sub++) {
                if (!((subs >> sub) & 1)) {
                    continue;
                }
                snprintf(name, sizeof(name), "h%d_%02x%02x%02x_%d", dev[i].kind, dev[i].mac3[0], dev[i].mac3[1],
                         dev[i].mac3[2], sub);
                hid = checked(form, name, c) ? hid & ~(1u << sub) : hid | (1u << sub);
            }
            hid_set(c, dev[i].kind, dev[i].mac3, hid);
        }
    }
    save();
    hub_expose_send_all();
}
