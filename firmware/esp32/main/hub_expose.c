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
#include "hub_upstream.h"
#include "hub_plugins.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_exp_mx;
static SemaphoreHandle_t s_exp_mx_get(void)
{
    if (!s_exp_mx) {
        s_exp_mx = xSemaphoreCreateRecursiveMutex();   /* 처음 부르는 곳 = app_main 의 load(다른 태스크 전) */
    }
    return s_exp_mx;
}
#define LOCK()   xSemaphoreTakeRecursive(s_exp_mx_get(), portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_exp_mx_get())
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

/* 옛 노출 설정(키 zb2/lan2/mt2: 비트 0 보일러 기기, 8/11/14/17/20/23/26 방 z1·z2·z3·z4~z7 기기 +1 켜기 +2 온도)을
 * 새 형식으로 옮긴다. 컨트롤 비트는 처음 읽을 때 한 번, 기기·방 숨김은 그 기기가 처음 보일 때 기기마다 한 번
 * (옮긴 기기는 "lgdone" 에 적어 두어, 나중에 사용자가 다시 보이게 바꾼 것을 되돌리지 않는다). */
static const char *const OLD_KEY[EXPOSE_NCH] = { "zb2", "lan2", "mt2" };
static const uint8_t OLD_ZBIT[8] = { 0, 8, 11, 14, 17, 20, 23, 26 };   /* z1~z7 의 옛 기기 비트 */
static uint32_t s_old[EXPOSE_NCH];
static bool s_has_old[EXPOSE_NCH];
#define MAX_LGDONE 8
#define OLD_KEEP_BOOTS 5                               /* 옮긴 뒤 옛 값을 남겨 두는 부팅 횟수 */
static uint8_t s_lgdone[MAX_LGDONE][4];                /* [종류][MAC끝3] */
static int s_nlgdone;
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
    { 5, "온수 켜기/끄기" }, { 6, "온수 온도" }, { 7, "온수 예열" }, { 13, "가스 요금 (이번달 사용량·예상 요금)" },
};
static const ctl_t ROOM_CTL[] = { { 8, "켜기/끄기" }, { 9, "설정 온도" }, { 10, "외출" }, { 12, "예약" } };
static const ctl_t HUB_CTL[] = { { EXPOSE_HUB_TEMP, "허브 내부 온도 (칩 온도 — 방 온도와 다름)" } };
/* 허브에 꽂은 센서(플러그인)의 기기: 설치된 것만 화면에 나오고, 화면에 나온 것만 저장 때 바뀐다 */
static const struct {
    ctl_t ctl;
    const char *plugin;
} PLUG_CTL[] = { { { EXPOSE_ROOM_DEV, "온습도계" }, "room" }, { { EXPOSE_GAS_DEV, "가스 계량기" }, "gas" } };
#define NP (int)(sizeof(PLUG_CTL) / sizeof(PLUG_CTL[0]))
#define NB (int)(sizeof(BOILER_CTL) / sizeof(BOILER_CTL[0]))
#define NR (int)(sizeof(ROOM_CTL) / sizeof(ROOM_CTL[0]))
#define NH (int)(sizeof(HUB_CTL) / sizeof(HUB_CTL[0]))

bool hub_expose_bit_on(int ch, int bit) { return ch >= 0 && ch < EXPOSE_NCH && ((s_mask[ch] >> bit) & 1); }

/* ---------------------------------------------------------------- 저장 */

/* 옛 마스크 → 새 컨트롤 마스크. 방 컨트롤은 옛 방들 중 하나라도 숨겼으면 숨김(숨긴 쪽 우선) */
static uint32_t migrate_mask(uint32_t old)
{
    uint32_t m = ALL_BITS;
    for (int b = 1; b <= 7; b++) {                       /* 보일러 컨트롤: 같은 비트 */
        if (!((old >> b) & 1)) {
            m &= ~(1u << b);
        }
    }
    for (int z = 1; z <= 7; z++) {
        if (!((old >> (OLD_ZBIT[z] + 1)) & 1)) {
            m &= ~(1u << 8);                              /* 방 켜기/끄기 */
        }
        if (!((old >> (OLD_ZBIT[z] + 2)) & 1)) {
            m &= ~(1u << 9);                              /* 방 설정 온도 */
        }
    }
    for (int b = EXPOSE_ROOM_DEV; b <= EXPOSE_GAS_DEV; b++) {   /* 플러그인 기기: 같은 비트 */
        if (!((old >> b) & 1)) {
            m &= ~(1u << b);
        }
    }
    return m;
}

static esp_err_t save(void);

static void hub_expose_load_l(void)
{
    nvs_handle_t h;
    bool migrated = false;
    if (nvs_open("expose", NVS_READWRITE, &h) == ESP_OK) {
        for (int c = 0; c < EXPOSE_NCH; c++) {
            bool have_new = hs_get_u32(h, "expose", CH_KEY[c], &s_mask[c]) == ESP_OK;
            size_t n = sizeof(s_hid[c]);
            if (hs_get_blob(h, "expose", HID_KEY[c], s_hid[c], &n) == ESP_OK && n % sizeof(hid_t) == 0) {
                s_nhid[c] = (int)(n / sizeof(hid_t));
            }
            uint32_t old;
            if (nvs_get_u32(h, OLD_KEY[c], &old) == ESP_OK) {   /* 옛 값(검사값 없이 읽음 — 옮기기 전용) */
                s_old[c] = old;
                s_has_old[c] = true;
                if (!have_new) {
                    s_mask[c] = migrate_mask(old);
                    migrated = true;
                    ESP_LOGW(TAG, "옛 노출 설정 옮김(%s): %08lx → 컨트롤 %08lx", CH_NAME[c], (unsigned long)old,
                             (unsigned long)s_mask[c]);
                }
            }
        }
        size_t n = sizeof(s_lgdone);
        if (nvs_get_blob(h, "lgdone", s_lgdone, &n) == ESP_OK) {
            s_nlgdone = (int)(n / 4);
        }
        if (s_nlgdone && !migrated) {
            /* 기기별로 옮긴 뒤 OLD_KEEP_BOOTS 번의 부팅 동안은 옛 값을 남겨 둔다 — 옮긴 부팅에 꺼져 있던 기기가
             * 나중에 붙어도 옛 숨김을 이어받게. 그 뒤엔 지운다(훗날 새로 산 기기에 옛 숨김이 걸리지 않게). */
            uint8_t boots = 0;
            nvs_get_u8(h, "lgboots", &boots);
            if (++boots >= OLD_KEEP_BOOTS) {
                for (int c = 0; c < EXPOSE_NCH; c++) {
                    nvs_erase_key(h, OLD_KEY[c]);
                    s_has_old[c] = false;
                }
                nvs_erase_key(h, "lgdone");
                nvs_erase_key(h, "lgboots");
                s_nlgdone = 0;
                ESP_LOGI(TAG, "옛 노출 설정 정리 끝");
            } else {
                nvs_set_u8(h, "lgboots", boots);
            }
            nvs_commit(h);
        }
        nvs_close(h);
    }
    if (migrated) {
        save();
    }
    ESP_LOGI(TAG, "노출: Zigbee %08lx(숨김 %d), LAN %08lx(숨김 %d)", (unsigned long)s_mask[0], s_nhid[0],
             (unsigned long)s_mask[1], s_nhid[1]);
}

static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("expose", NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    for (int c = 0; e == ESP_OK && c < EXPOSE_NCH; c++) {
        e = hs_set_u32(h, CH_KEY[c], s_mask[c]);
        if (e == ESP_OK && s_nhid[c]) {
            e = hs_set_blob(h, HID_KEY[c], s_hid[c], s_nhid[c] * sizeof(hid_t));
        } else if (e == ESP_OK) {
            hs_erase(h, HID_KEY[c]);
        }
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "노출 설정 저장 실패(%s)", esp_err_to_name(e));
    }
    return e;
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

static void hub_expose_forget_l(uint8_t kind, const uint8_t mac3[3])
{
    for (int c = 0; c < EXPOSE_NCH; c++) {
        hid_set(c, kind, mac3, 0);
    }
    save();
}

/* 옛 설정이 있으면, 처음 보이는 기기마다 옛 기기·방 숨김을 한 번 옮긴다 */
static void migrate_devices(void)
{
    bool any = false;
    for (int c = 0; c < EXPOSE_NCH; c++) {
        any |= s_has_old[c];
    }
    if (!any) {
        return;
    }
    hub_dev_t dev[8];
    int nd = hub_rinnai_snapshot(dev, 8);
    bool changed = false;
    for (int i = 0; i < nd && s_nlgdone < MAX_LGDONE; i++) {
        bool done = false;
        for (int k = 0; k < s_nlgdone && !done; k++) {
            done = s_lgdone[k][0] == dev[i].kind && !memcmp(&s_lgdone[k][1], dev[i].mac3, 3);
        }
        if (done) {
            continue;
        }
        for (int c = 0; c < EXPOSE_NCH; c++) {
            if (!s_has_old[c]) {
                continue;
            }
            uint8_t hid = hid_get(c, dev[i].kind, dev[i].mac3);
            if (dev[i].kind == HUB_KIND_F06 && !((s_old[c] >> 0) & 1)) {
                hid |= 1;                                    /* 옛 '보일러 기기' 꺼짐 */
            }
            for (int z = 1; dev[i].kind == HUB_KIND_F09 && z <= 7; z++) {
                if (!((s_old[c] >> OLD_ZBIT[z]) & 1)) {
                    hid |= 1u << z;                          /* 옛 '방 기기' 꺼짐 */
                }
            }
            hid_set(c, dev[i].kind, dev[i].mac3, hid);
        }
        s_lgdone[s_nlgdone][0] = dev[i].kind;
        memcpy(&s_lgdone[s_nlgdone][1], dev[i].mac3, 3);
        s_nlgdone++;
        changed = true;
    }
    if (changed && save() == ESP_OK) {               /* 숨김 정보를 먼저 저장하고(실패하면 표시도 안 남겨 다음에 다시) */
        nvs_handle_t h;
        if (nvs_open("expose", NVS_READWRITE, &h) == ESP_OK) {   /* 그다음 '옮김' 표시(사이에 꺼지면 다음에 다시 옮김) */
            nvs_set_blob(h, "lgdone", s_lgdone, s_nlgdone * 4);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGW(TAG, "옛 노출 설정을 기기별로 옮김(기기 %d대)", s_nlgdone);
    }
}

static void hub_expose_prepare_l(void) { migrate_devices(); }

static void hub_expose_send_all_l(void)
{
    migrate_devices();
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

static void hub_expose_render_types_l(char *o, size_t cap)
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
    strlcat(o, "</div><div class=grpbox><h3 class=grp>📡 LYNNAI 허브</h3>"
               "<p class='muted sub'>SmartThings 의 'LYNNAI Hub' 기기 자체</p>", cap);
    ctl_rows(o, cap, HUB_CTL, NH);
    bool any = false;
    for (int i = 0; i < NP; i++) {
        if (!hub_plugin_enabled(PLUG_CTL[i].plugin)) {
            continue;
        }
        if (!any) {
            strlcat(o, "</div><div class=grpbox><h3 class=grp>🧩 센서 (플러그인)</h3>"
                       "<p class='muted sub'>허브에 꽂은 센서의 기기 — 끄면 플랫폼에서 숨김(센서는 계속 읽음)</p>", cap);
            any = true;
        }
        ctl_rows(o, cap, &PLUG_CTL[i].ctl, 1);
    }
    strlcat(o, "</div><button class=sv>저장</button></form></div>", cap);
}

static void hub_expose_render_dev_l(char *o, size_t cap, uint8_t kind, const uint8_t mac3[3], int sub)
{
    char name[24];
    snprintf(name, sizeof(name), "h%d_%02x%02x%02x_%d", kind, mac3[0], mac3[1], mac3[2], sub);
    dev_ctx_t d = { kind, mac3, sub };
    boxes(o, cap, name, dev_on, &d);
}

/* 화면에 그린 기기·방을 폼에 적어 둔다 → 저장 때 그 사이 새로 붙은 기기를 '체크 안 됨=숨김'으로 오해하지 않게 */
static void hub_expose_render_dev_marker_l(char *o, size_t cap, uint8_t kind, const uint8_t mac3[3], uint8_t subs)
{
    char t[64];
    snprintf(t, sizeof(t), "<input type=hidden name=x%d_%02x%02x%02x value=%u>", kind, mac3[0], mac3[1], mac3[2], subs);
    strlcat(o, t, cap);
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

static bool hub_expose_apply_l(const char *form, char *err, size_t errlen)
{
    uint32_t scope = 0;
    for (int i = 0; i < NB; i++) {
        scope |= 1u << BOILER_CTL[i].bit;
    }
    for (int i = 0; i < NR; i++) {
        scope |= 1u << ROOM_CTL[i].bit;
    }
    for (int i = 0; i < NH; i++) {
        scope |= 1u << HUB_CTL[i].bit;
    }
    for (int i = 0; i < NP; i++) {                  /* 설치된(화면에 그린) 플러그인만 — 없는 것의 체크는 폼에 없다 */
        if (hub_plugin_enabled(PLUG_CTL[i].plugin)) {
            scope |= 1u << PLUG_CTL[i].ctl.bit;
        }
    }
    apply_bits(form, scope);
    esp_err_t e = save();
    ESP_LOGI(TAG, "컨트롤 노출 바뀜: Zigbee %08lx, LAN %08lx", (unsigned long)s_mask[0], (unsigned long)s_mask[1]);
    hub_expose_send_all();
    if (e != ESP_OK) {
        snprintf(err, errlen, "노출 설정을 저장하지 못했습니다(%s) — 재부팅하면 이전 값으로 돌아갑니다", esp_err_to_name(e));
        return false;
    }
    return true;
}

static bool hub_expose_apply_devices_l(const char *form)
{
    hub_dev_t dev[16];
    int nd = hub_rinnai_snapshot(dev, 8);
    nd += hub_upstream_snapshot(dev + nd, (int)(sizeof(dev) / sizeof(dev[0])) - nd);  /* 원격(위성) 기기 노출도 중심에서 */
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
    esp_err_t e = save();
    hub_expose_send_all();
    return e == ESP_OK;
}

/* ---------------------------------------------------------------- 잠금 래퍼
 * 관리 화면(웹 서버 태스크)이 쓰고, 상태 전송(hub_rinnai)·Zigbee 태스크가 읽는다 — 같은 뮤텍스로 감싼다.
 * 잠금 순서: hub_pairs → hub_expose (반대로 부르는 곳 없음). */
void hub_expose_load(void)
{
    LOCK();
    hub_expose_load_l();
    UNLOCK();
}

void hub_expose_forget(uint8_t kind, const uint8_t mac3[3])
{
    LOCK();
    hub_expose_forget_l(kind, mac3);
    UNLOCK();
}

void hub_expose_prepare(void)
{
    LOCK();
    hub_expose_prepare_l();
    UNLOCK();
}

void hub_expose_send_all(void)
{
    LOCK();
    hub_expose_send_all_l();
    UNLOCK();
}

void hub_expose_render_types(char *o, size_t cap)
{
    LOCK();
    hub_expose_render_types_l(o, cap);
    UNLOCK();
}

void hub_expose_render_dev(char *o, size_t cap, uint8_t kind, const uint8_t mac3[3], int sub)
{
    LOCK();
    hub_expose_render_dev_l(o, cap, kind, mac3, sub);
    UNLOCK();
}

void hub_expose_render_dev_marker(char *o, size_t cap, uint8_t kind, const uint8_t mac3[3], uint8_t subs)
{
    LOCK();
    hub_expose_render_dev_marker_l(o, cap, kind, mac3, subs);
    UNLOCK();
}

bool hub_expose_apply(const char *form, char *err, size_t errlen)
{
    LOCK();
    bool r = hub_expose_apply_l(form, err, errlen);
    UNLOCK();
    return r;
}

bool hub_expose_apply_devices(const char *form)
{
    LOCK();
    bool r = hub_expose_apply_devices_l(form);
    UNLOCK();
    return r;
}
