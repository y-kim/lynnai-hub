/*
 * hub_plugins.c -- 플러그인 표 + 설정 저장 + 관리 화면 조각. 설명은 hub_plugins.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "hub_store.h"

#include "hub_plugins.h"
#include "hub_expose.h"

static const char *TAG = "HUB_PLUGINS";

typedef struct {
    const char *key;        /* NVS 키 뒷부분 + 폼 이름 */
    const char *label;
    int def, min, max;
    bool pin;               /* GPIO 번호(허용 목록·겹침 검사) */
} field_t;

typedef struct {
    const char *id;
    uint8_t bit;
    const char *name;
    const char *help;
    bool def_on;            /* 처음(설정 없음)에 켜져 있는지 */
    int expose_bit;         /* hub_expose.h 의 '기기' 비트(노출 통로 고르기 = 숨기기) */
    field_t fields[4];
    int nfields;
} plugin_t;

static const plugin_t PLUGINS[] = {
    {
        .id = "room", .bit = PLUG_ROOM, .name = "온습도계 (SHT31)", .def_on = true, .expose_bit = EXPOSE_ROOM_DEV,
        .help = "I2C 주소 0x44. VIN → 3V3, GND → GND, SDA·SCL → 아래 핀. 보드형 모듈은 저항이 이미 있습니다.",
        .fields = {
            { "sda", "SDA 핀", 6, 0, 30, true },
            { "scl", "SCL 핀", 7, 0, 30, true },
            { "every", "재는 간격 (초)", 60, 10, 3600, false },
        },
        .nfields = 3,
    },
    {
        .id = "gas", .bit = PLUG_GAS, .name = "가스 계량기 (리드 스위치)", .def_on = true, .expose_bit = EXPOSE_GAS_DEV,
        .help = "스위치 한쪽 → 아래 핀, 다른 쪽 → GND (내부 풀업). 선이 길면 핀–3V3 사이 10k 저항 권장.",
        .fields = {
            { "pin", "스위치 핀", 10, 0, 30, true },
            { "step", "1펄스 양 (L, 계량기에 적힌 값: 0.01㎥ = 10)", 10, 1, 1000, false },
        },
        .nfields = 2,
    },
};
#define NPLUG (int)(sizeof(PLUGINS) / sizeof(PLUGINS[0]))

/* 보드(ESP32-C6-DevKitC-1)에 나와 있고 허브가 안 쓰는 핀.
 * 빠진 것: 2·3 외부 Zigbee 라디오(UART, 부팅 때 탐지), 8 LED, 9 버튼, 12·13 USB, 16·17 콘솔(UART), 24~30 플래시. */
static const int FREE_PINS[] = { 0, 1, 4, 5, 6, 7, 10, 11, 15, 18, 19, 20, 21, 22, 23 };

static bool s_on[NPLUG];
static int s_val[NPLUG][4];
static const char *s_act_label[NPLUG];
static const char *s_act_hint[NPLUG];
static hub_plugin_action_fn s_act_fn[NPLUG];

static const plugin_t *find(const char *id, int *idx)
{
    for (int i = 0; i < NPLUG; i++) {
        if (!strcmp(PLUGINS[i].id, id)) {
            if (idx) {
                *idx = i;
            }
            return &PLUGINS[i];
        }
    }
    return NULL;
}

void hub_plugin_register_action(const char *id, const char *label, const char *hint, hub_plugin_action_fn fn)
{
    int i;
    if (find(id, &i)) {
        s_act_label[i] = label;
        s_act_hint[i] = hint;
        s_act_fn[i] = fn;
    }
}

static void nvs_key(char *k, size_t n, const char *id, const char *sub) { snprintf(k, n, "%s.%s", id, sub); }

void hub_plugins_load(void)
{
    nvs_handle_t h;
    bool have = nvs_open("plug", NVS_READWRITE, &h) == ESP_OK;
    for (int i = 0; i < NPLUG; i++) {
        const plugin_t *p = &PLUGINS[i];
        char k[16];
        uint8_t en = p->def_on;
        nvs_key(k, sizeof(k), p->id, "en");
        if (have) {
            hs_get_u8(h, "plug", k, &en);
        }
        s_on[i] = en;
        for (int f = 0; f < p->nfields; f++) {
            int32_t v = p->fields[f].def;
            nvs_key(k, sizeof(k), p->id, p->fields[f].key);
            if (have) {
                hs_get_i32(h, "plug", k, &v);
            }
            s_val[i][f] = v;
        }
        ESP_LOGI(TAG, "%s: %s", p->name, s_on[i] ? "켜짐" : "꺼짐");
    }
    if (have) {
        nvs_close(h);
    }
}

bool hub_plugin_enabled(const char *id)
{
    int i;
    return find(id, &i) && s_on[i];
}

int hub_plugin_get(const char *id, const char *key)
{
    int i;
    const plugin_t *p = find(id, &i);
    if (!p) {
        return -1;
    }
    for (int f = 0; f < p->nfields; f++) {
        if (!strcmp(p->fields[f].key, key)) {
            return s_val[i][f];
        }
    }
    return -1;
}

uint8_t hub_plugins_mask(void)
{
    uint8_t m = 0;
    for (int i = 0; i < NPLUG; i++) {
        if (s_on[i]) {
            m |= PLUGINS[i].bit;
        }
    }
    return m;
}

/* ---------------------------------------------------------------- 화면 */

static void cat(char *o, size_t cap, const char *s) { strlcat(o, s, cap); }

void hub_plugins_render(char *o, size_t cap)
{
    char t[768];             /* 제목 줄(삭제 확인 문구 포함) + 설명이 한 번에 들어가게 */
    cat(o, cap, "<h2>설치된 플러그인</h2><p class=muted>노출을 모두 끄면 플랫폼에서 숨김(센서는 계속 읽음). "
                "🗑️ = 플러그인 제거.</p>");
    bool any = false;
    for (int i = 0; i < NPLUG; i++) {
        const plugin_t *p = &PLUGINS[i];
        if (!s_on[i]) {
            continue;
        }
        any = true;
        /* 제목 줄: 이름 + 오른쪽 끝 삭제(따로 된 작은 폼 — 설정 폼에서 Enter 가 삭제를 누르지 않게) */
        snprintf(t, sizeof(t), "<div class=card><div class=ptitle><h3>%s</h3>"
                 "<form method=post action=/admin/plugin><input type=hidden name=id value=%s>"
                 "<button name=op value=del class=ic title=\"플러그인 삭제\" "
                 "onclick=\"return confirm('플러그인을 삭제할까요? 센서를 더 이상 읽지 않고, 모든 플랫폼에서 기기가 빠집니다.')\">"
                 "\U0001f5d1️</button></form></div><p class=muted>%s</p>", p->name, p->id, p->help);
        cat(o, cap, t);
        /* 설정 + 노출을 한 폼, 저장 하나로 */
        snprintf(t, sizeof(t), "<form method=post action=/admin/plugin><input type=hidden name=id value=%s>", p->id);
        cat(o, cap, t);
        for (int f = 0; f < p->nfields; f++) {
            snprintf(t, sizeof(t), "<label>%s</label><input name=%s type=text inputmode=numeric pattern=\"[0-9]*\" "
                     "data-min=%d data-max=%d value=%d>",   /* 숫자 전용, 위아래 스피너 없음(min/max 는 서버 검증) */
                     p->fields[f].label, p->fields[f].key, p->fields[f].min, p->fields[f].max, s_val[i][f]);
            cat(o, cap, t);
        }
        cat(o, cap, "<div class=row><span>노출</span><span>");
        hub_expose_render_bit(o, cap, p->expose_bit);
        cat(o, cap, "</span></div><button name=op value=save class=sv>저장</button></form>");
        if (s_act_fn[i]) {                          /* 즉시 동작(예: 계량기 숫자 맞추기)은 따로 */
            snprintf(t, sizeof(t), "<form method=post action=/admin/plugin-action><input type=hidden name=id value=%s>"
                     "<label>%s</label><input name=value inputmode=decimal placeholder=\"%s\">"
                     "<button class=ghost>%s</button></form>", p->id, s_act_label[i], s_act_hint[i], s_act_label[i]);
            cat(o, cap, t);
        }
        cat(o, cap, "</div>");
    }
    if (!any) {
        cat(o, cap, "<p class=muted>없음</p>");
    }
    cat(o, cap, "<h2>추가할 수 있는 플러그인</h2>");
    any = false;
    for (int i = 0; i < NPLUG; i++) {
        const plugin_t *p = &PLUGINS[i];
        if (s_on[i]) {
            continue;
        }
        any = true;
        snprintf(t, sizeof(t), "<div class=card><h3>%s</h3><p class=muted>%s</p>"
                 "<form method=post action=/admin/plugin><input type=hidden name=id value=%s>", p->name, p->help, p->id);
        cat(o, cap, t);
        for (int f = 0; f < p->nfields; f++) {
            snprintf(t, sizeof(t), "<label>%s</label><input name=%s type=text inputmode=numeric pattern=\"[0-9]*\" "
                     "data-min=%d data-max=%d value=%d>",   /* 숫자 전용, 위아래 스피너 없음(min/max 는 서버 검증) */
                     p->fields[f].label, p->fields[f].key, p->fields[f].min, p->fields[f].max, s_val[i][f]);
            cat(o, cap, t);
        }
        cat(o, cap, "<button name=op value=add>추가</button></form></div>");
    }
    if (!any) {
        cat(o, cap, "<p class=muted>모두 설치됨</p>");
    }
    snprintf(t, sizeof(t), "<p class=muted>쓸 수 있는 핀:");
    cat(o, cap, t);
    for (size_t k = 0; k < sizeof(FREE_PINS) / sizeof(FREE_PINS[0]); k++) {
        snprintf(t, sizeof(t), " %d", FREE_PINS[k]);
        cat(o, cap, t);
    }
    cat(o, cap, " (보드에 인쇄된 숫자). 저장하면 센서만 다시 적용됩니다(보일러·각방 연결은 유지).</p>");
}

/* ---------------------------------------------------------------- 적용 */

static bool pin_free(int pin)
{
    for (size_t k = 0; k < sizeof(FREE_PINS) / sizeof(FREE_PINS[0]); k++) {
        if (FREE_PINS[k] == pin) {
            return true;
        }
    }
    return false;
}

static bool form_int(const char *form, const char *key, int *out)
{
    char v[16];
    if (httpd_query_key_value(form, key, v, sizeof(v)) != ESP_OK || !v[0]) {
        return false;
    }
    char *end;
    long n = strtol(v, &end, 10);
    if (*end) {
        return false;
    }
    *out = (int)n;
    return true;
}

bool hub_plugins_apply(const char *form, char *err, size_t errlen)
{
    char id[16], op[8];
    int idx;
    if (httpd_query_key_value(form, "id", id, sizeof(id)) != ESP_OK || !find(id, &idx) ||
        httpd_query_key_value(form, "op", op, sizeof(op)) != ESP_OK) {
        snprintf(err, errlen, "잘못된 요청");
        return false;
    }
    const plugin_t *p = &PLUGINS[idx];
    bool on = strcmp(op, "del") != 0;
    int vals[4];
    memcpy(vals, s_val[idx], sizeof(vals));
    if (on) {
        for (int f = 0; f < p->nfields; f++) {
            const field_t *fd = &p->fields[f];
            int v;
            if (!form_int(form, fd->key, &v) || v < fd->min || v > fd->max) {
                snprintf(err, errlen, "%s: %d~%d 사이 숫자", fd->label, fd->min, fd->max);
                return false;
            }
            if (fd->pin && !pin_free(v)) {
                snprintf(err, errlen, "%s: %d번은 쓸 수 없는 핀", fd->label, v);
                return false;
            }
            vals[f] = v;
        }
        /* 핀 겹침: 이 플러그인 안 + 켜진 다른 플러그인 */
        for (int f = 0; f < p->nfields; f++) {
            if (!p->fields[f].pin) {
                continue;
            }
            for (int g = f + 1; g < p->nfields; g++) {
                if (p->fields[g].pin && vals[g] == vals[f]) {
                    snprintf(err, errlen, "%d번 핀을 두 번 썼습니다", vals[f]);
                    return false;
                }
            }
            for (int j = 0; j < NPLUG; j++) {
                if (j == idx || !s_on[j]) {
                    continue;
                }
                for (int g = 0; g < PLUGINS[j].nfields; g++) {
                    if (PLUGINS[j].fields[g].pin && s_val[j][g] == vals[f]) {
                        snprintf(err, errlen, "%d번 핀은 '%s' 가 쓰고 있습니다", vals[f], PLUGINS[j].name);
                        return false;
                    }
                }
            }
        }
    }
    nvs_handle_t h;
    if (nvs_open("plug", NVS_READWRITE, &h) != ESP_OK) {
        snprintf(err, errlen, "저장소 열기 실패");
        return false;
    }
    char k[16];
    nvs_key(k, sizeof(k), p->id, "en");
    esp_err_t e = hs_set_u8(h, k, on);
    for (int f = 0; e == ESP_OK && f < p->nfields; f++) {
        nvs_key(k, sizeof(k), p->id, p->fields[f].key);
        e = hs_set_i32(h, k, vals[f]);
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK) {
        snprintf(err, errlen, "저장하지 못했습니다(%s)", esp_err_to_name(e));
        return false;
    }
    s_on[idx] = on;
    memcpy(s_val[idx], vals, sizeof(vals));
    if (!strcmp(op, "save")) {
        if (!hub_expose_apply_bit(form, p->expose_bit)) {   /* 같은 폼의 노출 체크(통로별) */
            snprintf(err, errlen, "노출 설정을 저장하지 못했습니다");
            return false;
        }
    }
    ESP_LOGI(TAG, "%s %s", p->name, !on ? "삭제" : !strcmp(op, "add") ? "추가" : "저장");
    return true;
}

bool hub_plugins_action(const char *form, char *msg, size_t msglen)
{
    char id[16], value[48];
    int idx;
    if (httpd_query_key_value(form, "id", id, sizeof(id)) != ESP_OK || !find(id, &idx) || !s_act_fn[idx]) {
        snprintf(msg, msglen, "잘못된 요청");
        return false;
    }
    if (httpd_query_key_value(form, "value", value, sizeof(value)) != ESP_OK || !value[0]) {
        snprintf(msg, msglen, "값이 필요합니다");
        return false;
    }
    /* 폼 값은 URL 인코딩: "1676,47" 은 "1676%2C47" 로 온다 */
    char *w = value;
    for (const char *r = value; *r; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (r[0] == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hx[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hx, NULL, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
    return s_act_fn[idx](value, msg, msglen);
}
