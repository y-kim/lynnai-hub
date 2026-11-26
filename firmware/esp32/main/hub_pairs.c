/*
 * hub_pairs.c -- 보일러↔써머스탯 짝 저장·화면·전송. 설명은 hub_pairs.h.
 * 저장: NVS "pairs", 키 = f09 MAC끝3(6hex), 값 = f06 MAC끝3(6hex). 최대 MAX_PAIRS.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs.h"
#include "hub_store.h"

#include "hub_pairs.h"
#include "hub_rinnai.h"
#include "hub_expose.h"
#include "hub_upstream.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static SemaphoreHandle_t s_pairs_mx;
static SemaphoreHandle_t s_pairs_mx_get(void)
{
    if (!s_pairs_mx) {
        s_pairs_mx = xSemaphoreCreateRecursiveMutex();   /* 처음 부르는 곳 = app_main 의 load(다른 태스크 전) */
    }
    return s_pairs_mx;
}
#define LOCK()   xSemaphoreTakeRecursive(s_pairs_mx_get(), portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_pairs_mx_get())
static const char *TAG = "HUB_PAIRS";
#define MAX_PAIRS 6

typedef struct {
    char f09[7];    /* 6 hex + NUL */
    char f06[7];
} pair_t;
static pair_t s_pairs[MAX_PAIRS];
static int s_n;

/* 별명(아래에 정의, hub_pairs_load 가 먼저 쓰므로 선언을 위로) */
#define MAX_ALIAS 8
#define ALIAS_LEN 64             /* UTF-8 바이트(한글 3바이트 → 약 20자, 팝업 maxlength=20) */
typedef struct { char mac[7]; char name[ALIAS_LEN]; } alias_t;
static alias_t s_alias[MAX_ALIAS];
static int s_na;
static void alias_load(void);

/* hub_rinnai: 짝이 바뀌면 다시 보내도록 깨운다(연결과 함께 전송됨) */
void hub_rinnai_resend(void);

static void mac_hex(const uint8_t m[3], char out[7]) { snprintf(out, 7, "%02x%02x%02x", m[0], m[1], m[2]); }

static void hub_pairs_load_l(void)
{
    nvs_handle_t h;
    s_n = 0;
    if (nvs_open("pairs", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < MAX_PAIRS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "p%d", i);
        size_t n = sizeof(s_pairs[0]);
        char buf[16];
        n = sizeof(buf);
        if (hs_get_str(h, "pairs", key, buf, &n) == ESP_OK && strlen(buf) == 13 && buf[6] == '>') {
            memcpy(s_pairs[s_n].f09, buf, 6);
            s_pairs[s_n].f09[6] = 0;
            memcpy(s_pairs[s_n].f06, buf + 7, 6);
            s_pairs[s_n].f06[6] = 0;
            s_n++;
        }
    }
    nvs_close(h);
    alias_load();
    ESP_LOGI(TAG, "짝 %d개, 별명 %d개 불러옴", s_n, s_na);
}

static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("pairs", NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    for (int i = 0; i < MAX_PAIRS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "p%d", i);
        if (i < s_n) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%s>%s", s_pairs[i].f09, s_pairs[i].f06);   /* "f09>f06" */
            if (e == ESP_OK) {
                e = hs_set_str(h, key, buf);
            }
        } else {
            hs_erase(h, key);
        }
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

/* ---------------------------------------------------------------- 별명 */
/* 기기 별명(허브 관리용). NVS "alias", 키 a0..aN, 값 "MAC끝3(6hex)=별명". ST 이름은 앱에서 바꿈. */
static void alias_load(void)
{
    nvs_handle_t h;
    s_na = 0;
    if (nvs_open("alias", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < MAX_ALIAS; i++) {
        char key[8], buf[7 + ALIAS_LEN];
        snprintf(key, sizeof(key), "a%d", i);
        size_t n = sizeof(buf);
        if (hs_get_str(h, "alias", key, buf, &n) == ESP_OK && strlen(buf) > 7 && buf[6] == '=') {
            memcpy(s_alias[s_na].mac, buf, 6);
            s_alias[s_na].mac[6] = 0;
            strlcpy(s_alias[s_na].name, buf + 7, ALIAS_LEN);
            s_na++;
        }
    }
    nvs_close(h);
}

static const char *alias_of(const char *mac)
{
    for (int i = 0; i < s_na; i++) {
        if (!strcmp(s_alias[i].mac, mac)) {
            return s_alias[i].name;
        }
    }
    return NULL;
}

/* UTF-8 글자 중간에서 잘리지 않게 복사(끝의 미완성 바이트 열은 버림) */
static void utf8_copy(char *dst, const char *src, size_t cap)
{
    strlcpy(dst, src, cap);
    size_t n = strlen(dst);
    if (n < strlen(src)) {                       /* 잘렸으면 마지막 글자가 온전한지 본다 */
        size_t i = n;
        while (i > 0 && ((unsigned char)dst[i - 1] & 0xC0) == 0x80) {
            i--;                                 /* 이어지는 바이트(10xxxxxx) 건너뜀 */
        }
        if (i > 0 && ((unsigned char)dst[i - 1] & 0x80)) {
            unsigned char lead = (unsigned char)dst[i - 1];
            size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (n - (i - 1) < want) {
                dst[i - 1] = 0;                  /* 시작 바이트부터 잘라냄 */
            }
        }
    }
}

static void alias_set(const char *mac, const char *name)
{
    for (int i = 0; i < s_na; i++) {
        if (!strcmp(s_alias[i].mac, mac)) {
            if (name && *name) {
                utf8_copy(s_alias[i].name, name, ALIAS_LEN);
            } else {
                s_alias[i] = s_alias[--s_na];
            }
            return;
        }
    }
    if (name && *name && s_na < MAX_ALIAS) {
        strlcpy(s_alias[s_na].mac, mac, 7);
        utf8_copy(s_alias[s_na].name, name, ALIAS_LEN);
        s_na++;
    }
}

static esp_err_t alias_save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("alias", NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    for (int i = 0; i < MAX_ALIAS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "a%d", i);
        if (i < s_na) {
            char buf[7 + ALIAS_LEN];
            snprintf(buf, sizeof(buf), "%s=%s", s_alias[i].mac, s_alias[i].name);
            if (e == ESP_OK) {
                e = hs_set_str(h, key, buf);
            }
        } else {
            hs_erase(h, key);
        }
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

/* 폼 값 URL 디코드(%XX, +) — 한글 별명용 */
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hx[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hx, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            *o++ = ' ';
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

static const char *paired_f06(const char *f09)
{
    for (int i = 0; i < s_n; i++) {
        if (!strcmp(s_pairs[i].f09, f09)) {
            return s_pairs[i].f06;
        }
    }
    return NULL;
}

static void set_pair(const char *f09, const char *f06)
{
    for (int i = 0; i < s_n; i++) {
        if (!strcmp(s_pairs[i].f09, f09)) {
            if (f06 && *f06) {
                strlcpy(s_pairs[i].f06, f06, 7);
            } else {                                /* 단독: 항목 제거 */
                s_pairs[i] = s_pairs[--s_n];
            }
            return;
        }
    }
    if (f06 && *f06 && s_n < MAX_PAIRS) {
        strlcpy(s_pairs[s_n].f09, f09, 7);
        strlcpy(s_pairs[s_n].f06, f06, 7);
        s_n++;
    }
}

static int hub_pairs_blob_l(uint8_t *out, int cap)
{
    int n = 0;
    for (int i = 0; i < s_n && n + 6 <= cap; i++) {
        for (int k = 0; k < 3; k++) {
            char hx[3] = { s_pairs[i].f09[2 * k], s_pairs[i].f09[2 * k + 1], 0 };
            out[n + k] = (uint8_t)strtoul(hx, NULL, 16);
            char hy[3] = { s_pairs[i].f06[2 * k], s_pairs[i].f06[2 * k + 1], 0 };
            out[n + 3 + k] = (uint8_t)strtoul(hy, NULL, 16);
        }
        n += 6;
    }
    return n;
}

/* ---------------------------------------------------------------- 화면 */

static void cat(char *o, size_t cap, const char *s) { strlcat(o, s, cap); }

/* 별명은 사용자가 넣은 글자라 HTML 속성·본문에 넣기 전에 이스케이프(따옴표·< 로 화면이 깨지지 않게) */
static const char *esc(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    for (; s && *s && n + 7 < cap; s++) {
        const char *r = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;"
                      : *s == '"' ? "&quot;" : *s == '\'' ? "&#39;" : NULL;
        if (r) {
            n += strlcpy(out + n, r, cap - n);
        } else {
            out[n++] = *s;
        }
    }
    out[n] = 0;
    return out;
}

/* 보일러 조절기 드롭다운에 보일 이름: 별명 있으면 별명, 없으면 "보일러 조절기 <mac>" */
static void boiler_label(const char *mac, char *out, size_t cap)
{
    const char *al = alias_of(mac);
    if (al) {
        strlcpy(out, al, cap);
    } else {
        snprintf(out, cap, "보일러 조절기 %s", mac);
    }
}

static void hub_pairs_render_l(char *o, size_t cap)
{
    hub_expose_prepare();                            /* 옛 노출 설정이 남아 있으면 먼저 기기별로 옮김 */
    hub_dev_t dev[16];
    int nd = hub_rinnai_snapshot(dev, 8);                 /* 로컬(이 허브에 직접 붙은) 기기 */
    int ntot = nd + hub_upstream_snapshot(dev + nd, (int)(sizeof(dev) / sizeof(dev[0])) - nd);  /* + 위성 허브 원격 기기 */
    char f06s[8][7];
    int n6 = 0;
    for (int i = 0; i < nd; i++) {                         /* 보일러 조절기는 로컬만(원격은 방만 올라옴) */
        if (dev[i].kind == HUB_KIND_F06 && n6 < 8) {
            mac_hex(dev[i].mac3, f06s[n6++]);
        }
    }
    cat(o, cap, "<h2>연결된 기기</h2><div class=card><p class=muted>"
                "이 허브와 위성 허브에 붙은 린나이 기기입니다. 보일러 연결·노출·이름은 모두 <b>여기(중심 허브)</b>에서 정합니다. "
                "이름(✏️)을 지으면 여기와 연결 선택에 그 이름으로 나오고(SmartThings 기기 이름은 ST 앱에서 바꿉니다), "
                "각 기기·방 옆 체크는 그 기기를 어느 플랫폼에 보일지입니다.</p>");
    char t[512], ea[ALIAS_LEN * 6];
    if (ntot == 0) {
        cat(o, cap, "<p class=muted>아직 연결된 기기가 없습니다.</p>");
    } else {
    cat(o, cap, "<form method=post action=/admin/devices>");
    bool mode_shown = false;
    for (int i = 0; i < ntot; i++) {
        bool is_remote = i >= nd;
        char lblp[80] = "";                               /* 원격이면 머리말에 층 라벨(" · 1층") */
        if (is_remote) {
            char flbl[25], elbl[64];
            if (hub_upstream_label_of(dev[i].kind, dev[i].mac3, flbl, sizeof(flbl)) > 0) {
                snprintf(lblp, sizeof(lblp), " · %s", esc(flbl, elbl, sizeof(elbl)));
            }
        }
        char mac[7];
        mac_hex(dev[i].mac3, mac);
        const char *al = alias_of(mac);
        esc(al ? al : "", ea, sizeof(ea));
        bool f09 = dev[i].kind == HUB_KIND_F09;
        const char *typ = dev[i].kind == HUB_KIND_F06 ? "보일러 조절기" : f09 ? "온도 조절기" : "기기";
        int rooms = 0;
        for (int z = 0; f09 && z < HUB_NROOMS; z++) {
            rooms += hub_room_present(&dev[i], z);
        }
        char meta[24] = "";
        if (f09) {
            snprintf(meta, sizeof(meta), " · 방 %d개", rooms);
        }
        /* 기기 줄: 종류·MAC·상태 + 이름 칩(팝업, 비우고 저장 = 기기 삭제) */
        snprintf(t, sizeof(t), "<div class=row><span><b>%s</b>%s · %s%s · %s</span><span>"
                 "<button type=button class=ic title=\"이름 바꾸기\" onclick=\"editAlias('%s')\">✏️ <span id=al_%s>",
                 typ, lblp, mac, meta, dev[i].linked ? "연결됨" : "끊김", mac, mac);
        cat(o, cap, t);
        cat(o, cap, al ? ea : "이름 없음");
        snprintf(t, sizeof(t), "</span></button><input name=a_%s value=\"", mac);
        cat(o, cap, t);
        cat(o, cap, ea);
        cat(o, cap, "\" hidden></span></div>");

        /* 하위 줄들 — 보일러 조절기: 기기 노출 / 온도 조절기: 보일러 연결 + 방(세부 기기)별 노출 */
        uint8_t subs = 0;
        if (!f09) {
            cat(o, cap, "<div class=row style='padding-left:18px'><span class=muted>↳ 노출</span><span>");
            hub_expose_render_dev(o, cap, dev[i].kind, dev[i].mac3, 0);
            cat(o, cap, "</span></div>");
            subs = 1;
            if (dev[i].kind == HUB_KIND_F06 && !mode_shown) {
                /* 보일러 모드(허브에 하나): 보일러 Wi-Fi 만 달린 집은 유선 조절기가 있는지 알 수 없어 사람이 정한다.
                 * 저장은 이 폼(/admin/devices)과 같이 — hub_portal.c pairs_post 가 bmode 를 처리한다 */
                uint8_t bm = hub_rinnai_boil_mode();
                mode_shown = true;
                cat(o, cap, "<div class=row style='padding-left:18px'><span class=muted>↳ 보일러 모드</span><span><select name=bmode>");
                if (!bm) {
                    cat(o, cap, "<option value=0 selected>정하지 않음 (조절기 연결로 짐작)</option>");
                }
                cat(o, cap, bm == 1 ? "<option value=1 selected>" : "<option value=1>");
                cat(o, cap, "난방수 — 방마다 온도 조절기</option>");
                cat(o, cap, bm == 2 ? "<option value=2 selected>" : "<option value=2>");
                cat(o, cap, "온돌·실내 — 보일러 조절기 하나</option></select></span></div>");
            }
        } else {
            const char *cur = paired_f06(mac);
            bool solo = cur && !strcmp(cur, "000000");
            snprintf(t, sizeof(t), "<div class=row style='padding-left:18px'><span class=muted>↳ 보일러 조절기 연결</span>"
                     "<span><select name=p_%s><option value=\"\"%s>자동</option><option value=000000%s>단독</option>",
                     mac, cur ? "" : " selected", solo ? " selected" : "");
            cat(o, cap, t);
            for (int k = 0; k < n6; k++) {
                char bl[ALIAS_LEN + 24], eb[sizeof(bl) * 6];
                boiler_label(f06s[k], bl, sizeof(bl));
                snprintf(t, sizeof(t), "<option value=%s%s>%s</option>", f06s[k],
                         (cur && !solo && !strcmp(cur, f06s[k])) ? " selected" : "", esc(bl, eb, sizeof(eb)));
                cat(o, cap, t);
            }
            if (cur && !solo) {                          /* 짝 보일러가 지금 목록에 없어도(꺼짐·재부팅 직후) 선택된 채로 */
                bool listed = false;
                for (int k = 0; k < n6; k++) {
                    listed |= !strcmp(cur, f06s[k]);
                }
                if (!listed) {
                    char bl[ALIAS_LEN + 24], eb[sizeof(bl) * 6];
                    boiler_label(cur, bl, sizeof(bl));
                    snprintf(t, sizeof(t), "<option value=%s selected>%s (지금 연결 안 됨)</option>", cur,
                             esc(bl, eb, sizeof(eb)));
                    cat(o, cap, t);
                }
            }
            cat(o, cap, "</select></span></div>");
            for (int z = 0; z < HUB_NROOMS; z++) {
                if (!hub_room_present(&dev[i], z)) {
                    continue;
                }
                snprintf(t, sizeof(t), "<div class=row style='padding-left:18px'><span class=muted>↳ %s</span><span>",
                         HUB_ROOMS[z].name);
                cat(o, cap, t);
                hub_expose_render_dev(o, cap, dev[i].kind, dev[i].mac3, HUB_ROOMS[z].z);
                cat(o, cap, "</span></div>");
                subs |= 1u << HUB_ROOMS[z].z;
            }
        }
        hub_expose_render_dev_marker(o, cap, dev[i].kind, dev[i].mac3, subs);
    }
    cat(o, cap, "<button class=sv>저장</button></form>");
    }
    cat(o, cap, "</div>");
}

/* ---------------------------------------------------------------- 적용 */

static bool hex6(const char *s) { for (int i = 0; i < 6; i++) if (!isxdigit((unsigned char)s[i])) return false; return s[6] == 0; }

static bool hub_pairs_apply_l(const char *form, char *msg, size_t msglen)
{
    hub_dev_t dev[16];
    int nd = hub_rinnai_snapshot(dev, 8);
    nd += hub_upstream_snapshot(dev + nd, (int)(sizeof(dev) / sizeof(dev[0])) - nd);  /* 원격(위성) 기기도 중심에서 이름·보일러연결을 정한다 */
    for (int i = 0; i < nd; i++) {
        char mac[7], key[12], val[ALIAS_LEN * 3 + 1];   /* URL 인코딩(%XX)은 3배 — 작으면 한글 별명이 잘림 오류로 무시됨 */
        mac_hex(dev[i].mac3, mac);
        /* 별명 */
        snprintf(key, sizeof(key), "a_%s", mac);
        if (httpd_query_key_value(form, key, val, sizeof(val)) == ESP_OK) {
            url_decode(val);
            alias_set(mac, val[0] ? val : NULL);
        }
        /* 보일러 조절기 연결(온도 조절기만) */
        if (dev[i].kind == HUB_KIND_F09) {
            char pv[12];
            snprintf(key, sizeof(key), "p_%s", mac);
            if (httpd_query_key_value(form, key, pv, sizeof(pv)) == ESP_OK && (!pv[0] || hex6(pv))) {
                set_pair(mac, pv[0] ? pv : NULL);
            }
        }
    }
    esp_err_t e1 = alias_save(), e2 = save();
    bool ex = hub_expose_apply_devices(form);        /* 기기·방별 노출(같은 폼) */
    hub_rinnai_resend();
    if (e1 != ESP_OK || e2 != ESP_OK || !ex) {
        snprintf(msg, msglen, "일부를 저장하지 못했습니다(%s) — 재부팅하면 이전 값으로 돌아갈 수 있습니다",
                 esp_err_to_name(e1 != ESP_OK ? e1 : e2 != ESP_OK ? e2 : ESP_FAIL));
        return false;
    }
    snprintf(msg, msglen, "저장했습니다");
    return true;
}

/* 기기 삭제: 폼의 del=<MAC6> 를 허브에서 잊고, 별명·짝도 지운다. */
static bool hub_pairs_delete_l(const char *form, char *msg, size_t msglen)
{
    char mac[8];
    if (httpd_query_key_value(form, "del", mac, sizeof(mac)) != ESP_OK || !hex6(mac)) {
        snprintf(msg, msglen, "잘못된 요청");
        return false;
    }
    hub_dev_t dev[8];                                /* 지우기 전에 종류를 알아 둔다(노출 항목 정리용) */
    int nd = hub_rinnai_snapshot(dev, 8);
    for (int i = 0; i < nd; i++) {
        char m[7];
        mac_hex(dev[i].mac3, m);
        if (!strcmp(m, mac)) {
            hub_expose_forget(dev[i].kind, dev[i].mac3);
        }
    }
    hub_rinnai_forget(mac);
    alias_set(mac, NULL);
    for (int i = 0; i < s_n;) {
        if (!strcmp(s_pairs[i].f09, mac) || !strcmp(s_pairs[i].f06, mac)) {
            s_pairs[i] = s_pairs[--s_n];
        } else {
            i++;
        }
    }
    esp_err_t e1 = alias_save(), e2 = save();
    if (e1 != ESP_OK || e2 != ESP_OK) {
        snprintf(msg, msglen, "삭제를 저장하지 못했습니다(%s)", esp_err_to_name(e1 != ESP_OK ? e1 : e2));
        return false;
    }
    snprintf(msg, msglen, "기기를 삭제했습니다");
    return true;
}

/* ---------------------------------------------------------------- 잠금 래퍼
 * 관리 화면(웹 서버 태스크)이 쓰고, 상태 전송(hub_rinnai)·Zigbee 태스크가 읽는다 — 같은 뮤텍스로 감싼다.
 * 잠금 순서: hub_pairs → hub_expose (반대로 부르는 곳 없음). */
void hub_pairs_load(void)
{
    LOCK();
    hub_pairs_load_l();
    UNLOCK();
}

int hub_pairs_blob(uint8_t *out, int cap)
{
    LOCK();
    int r = hub_pairs_blob_l(out, cap);
    UNLOCK();
    return r;
}

void hub_pairs_render(char *o, size_t cap)
{
    LOCK();
    hub_pairs_render_l(o, cap);
    UNLOCK();
}

bool hub_pairs_apply(const char *form, char *msg, size_t msglen)
{
    LOCK();
    bool r = hub_pairs_apply_l(form, msg, msglen);
    UNLOCK();
    return r;
}

bool hub_pairs_delete(const char *form, char *msg, size_t msglen)
{
    LOCK();
    bool r = hub_pairs_delete_l(form, msg, msglen);
    UNLOCK();
    return r;
}
