/*
 * hub_expose.c -- 항목별 노출 통로. 설명·비트는 hub_expose.h.
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

/* 키 뒤 '2': 방 비트 배치가 바뀌어 옛 저장값은 뜻이 달라졌으므로 무시하고 전부 보임으로 시작 */
static const char *const CH_KEY[EXPOSE_NCH] = { "zb2", "lan2", "mt2" };
static const char *const CH_NAME[EXPOSE_NCH] = { "Zigbee", "LAN", "Matter" };
static const uint8_t CH_UPLINK[EXPOSE_NCH] = { HUB_UPLINK_ZIGBEE, HUB_UPLINK_LAN, HUB_UPLINK_MATTER };

#define ALL_BITS 0x7FFFFFFFu   /* 비트 0~30 전부(보일러 8 + 방 7×3 + 온습도 + 가스) */
static uint32_t s_mask[EXPOSE_NCH] = { ALL_BITS, ALL_BITS, ALL_BITS };

/* 린나이 항목 표. group = 소속 기기 이름, label = 세부 컨트롤 이름(NULL 이면 '기기 노출' 행). */
typedef struct {
    int bit;
    const char *group;
    const char *label;
} item_t;

static const item_t RINNAI[] = {
    { 0, "보일러", NULL },
    { 1, "보일러", "전원" },
    { 2, "보일러", "난방 모드 (난방/외출/끄기)" },
    { 3, "보일러", "난방 방식 (온돌/실내)" },
    { 4, "보일러", "난방 온도" },
    { 5, "보일러", "온수 켜기/끄기" },
    { 6, "보일러", "온수 온도" },
    { 7, "보일러", "온수 예열" },
    { 8, "방1", NULL },
    { 9, "방1", "켜기/끄기" },
    { 10, "방1", "설정 온도" },
    { 11, "방2", NULL },
    { 12, "방2", "켜기/끄기" },
    { 13, "방2", "설정 온도" },
    { 17, "방3", NULL },
    { 18, "방3", "켜기/끄기" },
    { 19, "방3", "설정 온도" },
    { 20, "방4", NULL },
    { 21, "방4", "켜기/끄기" },
    { 22, "방4", "설정 온도" },
    { 23, "방5", NULL },
    { 24, "방5", "켜기/끄기" },
    { 25, "방5", "설정 온도" },
    { 26, "방6", NULL },
    { 27, "방6", "켜기/끄기" },
    { 28, "방6", "설정 온도" },
    { 14, "거실", NULL },
    { 15, "거실", "켜기/끄기" },
    { 16, "거실", "설정 온도" },
};
#define NRINNAI (int)(sizeof(RINNAI) / sizeof(RINNAI[0]))

void hub_expose_load(void)
{
    nvs_handle_t h;
    if (nvs_open("expose", NVS_READWRITE, &h) == ESP_OK) {
        for (int c = 0; c < EXPOSE_NCH; c++) {
            hs_get_u32(h, "expose", CH_KEY[c], &s_mask[c]);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "노출: Zigbee %05lx, LAN %05lx, Matter %05lx", (unsigned long)s_mask[0], (unsigned long)s_mask[1],
             (unsigned long)s_mask[2]);
}

uint32_t hub_expose_mask(int ch) { return ch >= 0 && ch < EXPOSE_NCH ? s_mask[ch] : 0; }

void hub_expose_send_all(void)
{
    for (int c = 0; c < EXPOSE_NCH; c++) {
        uint32_t m = s_mask[c];
        hub_uplink_send_to(CH_UPLINK[c], HUB_RK_CMD_EXPOSE, (const uint8_t *)&m, sizeof(m));   /* 리틀엔디언 u32 */
    }
}

/* ---------------------------------------------------------------- 화면 */

/* 방 행은 온도 조절기가 실제로 리포트하는 슬롯만 보인다(stat·현재온도 다 0 = 그 슬롯에 방 없음, 드라이버와 같은 기준).
 * 보일러 행(슬롯 없음)은 늘 보인다. 화면과 저장이 같은 기준을 써야 숨은 방의 비트를 안 건드린다. */
static int group_slot(const char *g)
{
    static const struct { const char *n; int s; } M[] = {
        { "방1", 0 }, { "방2", 7 }, { "방3", 14 }, { "방4", 21 }, { "방5", 28 }, { "방6", 35 }, { "거실", 42 },
    };
    for (int i = 0; i < (int)(sizeof(M) / sizeof(M[0])); i++) {
        if (!strcmp(g, M[i].n)) {
            return M[i].s;
        }
    }
    return -1;
}

static void presence(bool pres[NRINNAI])
{
    hub_dev_t d[6];
    int n = hub_rinnai_snapshot(d, 6);
    for (int i = 0; i < NRINNAI; i++) {
        int s = group_slot(RINNAI[i].group);
        pres[i] = s < 0;
        for (int k = 0; s >= 0 && k < n && !pres[i]; k++) {
            if (d[k].kind == HUB_KIND_F09 && s + 3 < d[k].n && (d[k].blob[s] || d[k].blob[s + 3])) {
                pres[i] = true;
            }
        }
    }
}

/* 사용하기로 한 통로(hub_uplink_mask)의 열만 보여 준다. */
static void boxes(char *o, size_t cap, int bit)
{
    char t[160];
    uint8_t active = hub_uplink_mask();
    for (int c = 0; c < EXPOSE_NCH; c++) {
        if (!(active & CH_UPLINK[c])) {
            continue;
        }
        snprintf(t, sizeof(t), "<label class=chk><input type=checkbox name=b%d_%d%s> %s</label>", bit, c,
                 (s_mask[c] >> bit) & 1 ? " checked" : "", CH_NAME[c]);
        strlcat(o, t, cap);
    }
}

void hub_expose_render_rinnai(char *o, size_t cap)
{
    char t[200];
    bool pres[NRINNAI];
    presence(pres);
    strlcat(o, "<h2>노출 설정</h2><div class=card><p class=muted>"
               "<b>기기 노출</b>은 그 기기를 플랫폼(SmartThings 등)에 통째로 보일지, "
               "<b>세부 노출</b>은 각 기기의 컨트롤을 하나씩 보일지입니다. 끄면 숨김(허브는 계속 중계·제어). "
               "방은 온도 조절기가 알려 주는 방만 나옵니다.</p>"
               "<form method=post action=/admin/expose><input type=hidden name=scope value=rinnai>", cap);
    /* 1) 기기 노출: 기기 행(label == NULL) */
    strlcat(o, "<h3 class=grp>기기 노출</h3>", cap);
    for (int i = 0; i < NRINNAI; i++) {
        if (RINNAI[i].label || !pres[i]) {
            continue;
        }
        snprintf(t, sizeof(t), "<div class=row><span>%s</span><span>", RINNAI[i].group);
        strlcat(o, t, cap);
        boxes(o, cap, RINNAI[i].bit);
        strlcat(o, "</span></div>", cap);
    }
    /* 2) 세부 노출: 컨트롤 행(기기별 소제목) */
    strlcat(o, "<h3 class=grp>세부 노출</h3>", cap);
    const char *cur = NULL;
    for (int i = 0; i < NRINNAI; i++) {
        if (!RINNAI[i].label || !pres[i]) {
            continue;
        }
        if (!cur || strcmp(cur, RINNAI[i].group)) {
            cur = RINNAI[i].group;
            snprintf(t, sizeof(t), "<p class=muted style='margin:12px 0 2px'>%s</p>", cur);
            strlcat(o, t, cap);
        }
        snprintf(t, sizeof(t), "<div class=row><span>%s</span><span>", RINNAI[i].label);
        strlcat(o, t, cap);
        boxes(o, cap, RINNAI[i].bit);
        strlcat(o, "</span></div>", cap);
    }
    strlcat(o, "<button class=sv>노출 저장</button></form></div>", cap);
}

void hub_expose_render_bit(char *o, size_t cap, int bit)
{
    char t[192];
    snprintf(t, sizeof(t), "<form method=post action=/admin/expose><input type=hidden name=scope value=%d>"
             "<div class=row><span>노출</span><span>", bit);
    strlcat(o, t, cap);
    boxes(o, cap, bit);
    strlcat(o, "</span></div><button class=sv>노출 저장 (모두 끄면 숨기기)</button></form>", cap);
}

/* ---------------------------------------------------------------- 적용 */

bool hub_expose_apply(const char *form, char *err, size_t errlen)
{
    char scope[12], key[12], v[8];
    if (httpd_query_key_value(form, "scope", scope, sizeof(scope)) != ESP_OK) {
        snprintf(err, errlen, "잘못된 요청");
        return false;
    }
    /* 이 폼이 다루는 비트들: 린나이 표 전체, 또는 플러그인 한 비트 */
    uint32_t scope_bits = 0;
    if (!strcmp(scope, "rinnai")) {
        bool pres[NRINNAI];
        presence(pres);                                   /* 화면에 없던 방의 비트는 그대로 둔다 */
        for (int i = 0; i < NRINNAI; i++) {
            if (pres[i]) {
                scope_bits |= 1u << RINNAI[i].bit;
            }
        }
    } else {
        int b = atoi(scope);
        if (b != EXPOSE_ROOM_DEV && b != EXPOSE_GAS_DEV) {
            snprintf(err, errlen, "잘못된 항목");
            return false;
        }
        scope_bits = 1u << b;
    }
    uint8_t active = hub_uplink_mask();
    uint32_t next[EXPOSE_NCH];
    for (int c = 0; c < EXPOSE_NCH; c++) {
        next[c] = s_mask[c];
        if (!(active & CH_UPLINK[c])) {
            continue;                                       /* 안 쓰는 통로(Matter 포함): 화면에 없으니 그대로 */
        }
        for (int b = 0; b < 32; b++) {
            if (!((scope_bits >> b) & 1)) {
                continue;
            }
            snprintf(key, sizeof(key), "b%d_%d", b, c);
            bool on = httpd_query_key_value(form, key, v, sizeof(v)) == ESP_OK;   /* 체크된 것만 온다 */
            next[c] = on ? next[c] | (1u << b) : next[c] & ~(1u << b);
        }
    }
    nvs_handle_t h;
    if (nvs_open("expose", NVS_READWRITE, &h) != ESP_OK) {
        snprintf(err, errlen, "저장소 열기 실패");
        return false;
    }
    for (int c = 0; c < EXPOSE_NCH; c++) {
        s_mask[c] = next[c];
        hs_set_u32(h, CH_KEY[c], next[c]);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "노출 바뀜: Zigbee %05lx, LAN %05lx", (unsigned long)s_mask[0], (unsigned long)s_mask[1]);
    hub_expose_send_all();
    return true;
}
