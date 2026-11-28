/*
 * hub_api.c -- 서버(NAS 기록기, Prometheus/Grafana, Home Assistant 등)가 읽어 가는 인터페이스.
 *
 *   GET /api/v1/state   지금 상태 전체(JSON). 보일러·각방은 해독한 값 + 원본 16진(raw)
 *   GET /metrics        같은 값을 Prometheus 텍스트 형식으로(장기 저장·그래프는 서버 쪽 도구가)
 *   GET /api/events     연결·가스 펄스 등 이벤트(hub_events.c) — 가스 사용량을 정확한 시각으로 다시 셀 때
 *
 * 인증: 관리 화면에서 API 토큰을 만들면 위 읽기 API 전부가 'Authorization: Bearer <토큰>' 을 요구한다.
 *       토큰이 없으면 집 LAN 에서 누구나 읽을 수 있다(처음 상태).
 * 시간: SNTP 로 맞춘 유닉스 시각을 "time" 에 넣는다(맞추기 전엔 null). 이벤트는 부팅 후 ms(up) 기준.
 *
 * 가스 사용량은 누적 카운터(gas.pulses / lynnai_gas_liters_total)로 준다 — 서버가 아무 간격으로 읽어도
 * 차이를 내면 그 사이 사용량이 정확히 나온다(놓친 구간도 포함).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "hub_store.h"

#include "hub_api.h"
#include "hub_rinnai.h"
#include "hub_upstream.h"
#include "hub_lan.h"
#include "hub_sensors.h"
#include "hub_tariff.h"
#include "hub_proxy.h"
#include "hub_diag.h"
#include "hub_dbgkey.h"
#include "hub_trace.h"
#include "hub_bbox.h"
#include "hub_ota.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "hub_plugins.h"

static const char *TAG = "HUB_API";

static char s_token[33];
static const char *s_id = "";

/* 각방 슬롯(드라이버 core.lua ZONES 와 같다): 상태·현재·설정 바이트 위치 */
static const struct {
    const char *id, *name;
    int stat, cur, set;
} ZONES[] = {                       /* 화면 순서. id 는 드라이버 존 번호(z3 = 거실은 옛 배치 유지) */
    { "z1", "방1", 0, 3, 4 },
    { "z2", "방2", 7, 10, 11 },
    { "z4", "방3", 14, 17, 18 },
    { "z5", "방4", 21, 24, 25 },
    { "z6", "방5", 28, 31, 32 },
    { "z7", "방6", 35, 38, 39 },
    { "z3", "거실", 42, 45, 46 },
};
#define NZONES 7
/* 지금 응답하는 방만(stat 비트0 0x01 = 이 자리에 조절기가 지금 답함 — 린나이 서버/앱과 같은 기준, 드라이버와 같음).
 * 방 번호를 바꾸면 뺀 자리에 찌꺼기(0x42 처럼 켜짐·난방 비트만 남거나 현재온도만 남음)가 남는데, 비트0 이 꺼져 있어
 * 방으로 세지 않는다 */
static bool zone_present(const uint8_t *b, int n, int z) { return ZONES[z].set < n && (b[ZONES[z].stat] & 0x01); }
#define ZONE_JSON "{\"id\":\"%s\",\"name\":\"%s\",\"on\":%s,\"heating\":%s,\"outing\":%s,\"reserve\":%s,\"interval\":%d,\"current\":%d,\"target\":%d}"
#define ZONE_ARGS(b, z) ZONES[z].id, ZONES[z].name, (b)[ZONES[z].stat] & 0x02 ? "true" : "false", \
    (b)[ZONES[z].stat] & 0x40 ? "true" : "false", (b)[ZONES[z].stat] & 0x10 ? "true" : "false", \
    (b)[ZONES[z].stat] & 0x08 ? "true" : "false", (b)[ZONES[z].stat + 1], (b)[ZONES[z].cur], (b)[ZONES[z].set]

/* ---------------------------------------------------------------- 토큰 */

static bool s_token_bad;   /* 토큰을 읽지 못함(없음과 다름) → 인증이 풀리지 않게 API 를 막는다 */

static void token_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof(s_token);
    s_token[0] = 0;
    s_token_bad = false;
    esp_err_t e = nvs_open("api", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = hs_get_str(h, "api", "token", s_token, &n);
        nvs_close(h);
    }
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        s_token[0] = 0;                              /* 토큰을 안 만든 상태: API 는 LAN 에 열림(관리 화면에서 고름) */
    } else if (e != ESP_OK) {
        s_token[0] = 0;
        s_token_bad = true;
        ESP_LOGE(TAG, "API 토큰을 읽지 못함(%s) — API 를 막음(관리 화면에서 새로 만들면 풀림)", esp_err_to_name(e));
    }
}

/* tok 를 저장하고(빈 문자열 = 끔) 성공해야 바꾼다 */
static esp_err_t token_store(const char *tok)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("api", NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = hs_set_str(h, "token", tok);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e == ESP_OK) {
        strlcpy(s_token, tok, sizeof(s_token));
        s_token_bad = false;
    } else {
        ESP_LOGE(TAG, "API 토큰 저장 실패(%s) — 이전 상태 유지", esp_err_to_name(e));
    }
    return e;
}

bool hub_api_has_token(void) { return s_token[0] != 0; }

bool hub_api_token_new(char out[33])
{
    uint8_t r[16];
    char t[33];
    esp_fill_random(r, sizeof(r));
    for (int i = 0; i < 16; i++) {
        snprintf(t + 2 * i, 3, "%02x", r[i]);
    }
    if (token_store(t) != ESP_OK) {
        return false;
    }
    memcpy(out, s_token, 33);
    ESP_LOGI(TAG, "API 토큰 새로 만듦");
    return true;
}

bool hub_api_token_clear(void)
{
    if (token_store("") != ESP_OK) {
        return false;
    }
    ESP_LOGI(TAG, "API 토큰 끔(LAN 에 열림)");
    return true;
}

bool hub_api_authorized(httpd_req_t *req)
{
    if (s_token_bad) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "API 토큰을 읽지 못해 막아 둠 (관리 화면 → 서버 연동에서 새로 만들기)");
        return false;
    }
    if (!s_token[0]) {
        return true;
    }
    char h[80];
    if (httpd_req_get_hdr_value_str(req, "Authorization", h, sizeof(h)) != ESP_OK || strncmp(h, "Bearer ", 7) ||
        strlen(h + 7) != 32) {
        goto deny;
    }
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) {
        d |= (uint8_t)(h[7 + i] ^ s_token[i]);
    }
    if (!d) {
        return true;
    }
deny:
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
    httpd_resp_sendstr(req, "API 토큰 필요 (관리 화면 → 서버 연동)");
    return false;
}

/* 읽기 API 접근 허용: 서버 API 토큰 또는 원격 디버그 키. 디버그 키부터 조용히 확인(맞으면 401 안 보냄). */
static bool api_or_dbg(httpd_req_t *req)
{
    return hub_dbgkey_check(req) || hub_api_authorized(req);
}

/* ---------------------------------------------------------------- 해독 */

typedef struct {
    bool ok, power, heat, outing, hotwater, preheat, indoor;
    bool heat_burn, hw_burn;        /* 실제 연소 중(난방/온수) — 스펙 heatCombustionStatus/hotWaterCombustionStatus */
    int hw_set, ondol_set, room_set;
} boiler_t;

/* f06 상태 문자열(16진) 의 i 번째 글자 값 — habridge decode_boiler 와 같은 위치 */
static int nib(const uint8_t *b, int i) { return i % 2 ? b[i / 2] & 0xf : b[i / 2] >> 4; }

static boiler_t decode_boiler(const uint8_t *b, int n)
{
    boiler_t r = { 0 };
    if (n < 16) {
        return r;
    }
    r.ok = true;
    r.power = nib(b, 0) == 1;
    r.heat = nib(b, 2) == 1;
    r.hotwater = nib(b, 3) == 1;
    r.heat_burn = nib(b, 8) == 1;   /* 난방 연소 중 */
    r.hw_burn = nib(b, 9) == 1;     /* 온수 연소 중 */
    r.preheat = nib(b, 5) == 1;
    r.indoor = nib(b, 7) == 1;
    r.outing = nib(b, 19) == 1;
    r.hw_set = nib(b, 10) * 16 + nib(b, 11);
    r.ondol_set = nib(b, 13) * 16 + nib(b, 14);
    r.room_set = nib(b, 15) * 16 + nib(b, 16);
    return r;
}

bool hub_api_heating(void)
{
    hub_dev_t dev[6];
    int nd = hub_rinnai_snapshot(dev, 6);
    bool heating = false, boiler = false, boiler_on = false;
    for (int i = 0; i < nd; i++) {
        if (!dev[i].linked) {
            continue;                              /* 끊긴 기기의 옛 상태로 판단하지 않는다 */
        }
        if (dev[i].kind == HUB_KIND_F06) {
            boiler_t b = decode_boiler(dev[i].blob, dev[i].n);
            if (b.ok) {
                boiler = true;
                boiler_on = boiler_on || (b.power && b.heat);
            }
        } else if (dev[i].kind == HUB_KIND_F09) {
            for (int z = 0; z < NZONES; z++) {
                if (zone_present(dev[i].blob, dev[i].n, z) && (dev[i].blob[ZONES[z].stat] & 0x42) == 0x42) {
                    heating = true;                /* 켜짐(0x02) + 난방 중(0x40) */
                }
            }
        }
    }
    return heating && (!boiler || boiler_on);
}

int hub_api_burning(void)
{
    hub_dev_t dev[6];
    int nd = hub_rinnai_snapshot(dev, 6), r = -1;
    for (int i = 0; i < nd; i++) {
        if (dev[i].linked && dev[i].kind == HUB_KIND_F06) {
            boiler_t b = decode_boiler(dev[i].blob, dev[i].n);
            if (b.ok) {
                r = (r > 0 ? r : 0) | (b.heat_burn ? 1 : 0) | (b.hw_burn ? 2 : 0);
            }
        }
    }
    return r;
}

static void hex(char *out, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", b[i]);
    }
    out[2 * n] = 0;
}

static long now_epoch(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (long)t : 0;
}

static int rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

/* ---------------------------------------------------------------- /api/v1/state */

/* 1단계 과도기: 기기 목록에서 '첫 보일러·첫 써머스탯'만 뽑아 기존 단일 형식으로 준다(기록기 호환).
 * 여러 대 전체를 JSON 배열로 내는 건 다음 단계. */
static void legacy_snapshot(uint8_t *f06, int *n06, uint8_t *f09, int *n09, uint8_t *links)
{
    hub_dev_t dev[6];
    int nd = hub_rinnai_snapshot(dev, 6);
    *n06 = *n09 = 0;
    *links = 0;
    for (int i = 0; i < nd; i++) {
        if (dev[i].kind == HUB_KIND_F06) {
            if (dev[i].linked) {
                *links |= 1;
            }
            if (!*n06) {
                *n06 = dev[i].n;
                memcpy(f06, dev[i].blob, dev[i].n);
            }
        } else if (dev[i].kind == HUB_KIND_F09) {
            if (dev[i].linked) {
                *links |= 2;
            }
            if (!*n09) {
                *n09 = dev[i].n;
                memcpy(f09, dev[i].blob, dev[i].n);
            }
        }
    }
}

/* "model":"23" (토픽의 모델코드 두 자리) — 모르면 null. 앱은 이것으로 온수·난방수 온도 표를 고른다 */
static void model_json(httpd_req_t *req, const hub_dev_t *d)
{
    char m[24];
    if (d->model >= 0) {
        snprintf(m, sizeof(m), "\"model\":\"%02x\"", d->model);
    } else {
        snprintf(m, sizeof(m), "\"model\":null");
    }
    httpd_resp_sendstr_chunk(req, m);
}

/* 그 서머스탯을 품은 허브 라벨(층) — 없으면 "label":null. "·\\ 은 뺀다(JSON 안전) */
static void label_json(httpd_req_t *req, uint8_t kind, const uint8_t *mac3)
{
    char lbl[32], out[48];
    int n = hub_upstream_label_of(kind, mac3, lbl, sizeof(lbl));
    if (n <= 0) {
        httpd_resp_sendstr_chunk(req, "\"label\":null,");
        return;
    }
    int k = 0;
    out[k++] = '"';
    for (int i = 0; lbl[i] && k < (int)sizeof(out) - 4; i++) {
        if (lbl[i] != '"' && lbl[i] != '\\') {
            out[k++] = lbl[i];
        }
    }
    out[k++] = '"';
    out[k++] = ',';
    out[k] = 0;
    httpd_resp_sendstr_chunk(req, "\"label\":");
    httpd_resp_sendstr_chunk(req, out);
}

#define CHUNK(...) do { snprintf(line, sizeof(line), __VA_ARGS__); httpd_resp_sendstr_chunk(req, line); } while (0)

static esp_err_t state_get(httpd_req_t *req)
{
    if (!api_or_dbg(req)) {
        return ESP_OK;
    }
    uint8_t f06[64], f09[64], links;
    int n06, n09;
    legacy_snapshot(f06, &n06, f09, &n09, &links);
    char line[256], raw[132];
    long t = now_epoch();
    httpd_resp_set_type(req, "application/json");

    CHUNK("{\"v\":1,\"id\":\"%s\",\"up_s\":%lld,\"up_ms\":%llu,", s_id, (long long)(esp_timer_get_time() / 1000000),
          (unsigned long long)hub_trace_now_ms());
    if (t) {
        CHUNK("\"time\":%ld,", t);
    } else {
        CHUNK("\"time\":null,");
    }
    int stk_link = 0, stk_up = 0;
    hub_proxy_stack_free(&stk_link, &stk_up);       /* 연결 태스크 스택 최소 여유 — 스택 크기를 줄여도 되는지 보는 값 */
    CHUNK("\"hub\":{\"heap\":%lu,\"heap_min\":%lu,\"rssi\":%d,\"plugins\":%u,\"sensor_faults\":%u,"
          "\"stk_link_free\":%d,\"stk_up_free\":%d,", (unsigned long)esp_get_free_heap_size(),
          (unsigned long)esp_get_minimum_free_heap_size(), rssi(), hub_plugins_mask(), hub_sensors_faults(), stk_link, stk_up);
    {
        /* 어떤 펌웨어가 어느 칸에서 돌고 있나(OTA 뒤 확인용) + 시험 부팅·되돌림 상태 */
        const esp_app_desc_t *ad = esp_app_get_description();
        const esp_partition_t *run = esp_ota_get_running_partition();
        int trial = 0;
        bool rb = false;
        hub_ota_state(&trial, &rb);
        CHUNK("\"fw_build\":\"%s %s\",\"slot\":\"%s\",\"ota_trial\":%d,\"ota_rolled_back\":%s},", ad->date, ad->time,
              run ? run->label : "?", trial, rb ? "true" : "false");
    }
    CHUNK("\"links\":{\"boiler\":%s,\"rooms\":%s},", links & 1 ? "true" : "false", links & 2 ? "true" : "false");
    {
        int gon = 0, gtot = hub_upstream_guest_count(&gon);
        CHUNK("\"upstream\":{\"mode\":%d,\"guest_online\":%s,\"guest_status\":\"%s\",\"guests_total\":%d,\"guests_online\":%d},",
              hub_upstream_mode(), hub_lan_guest_online() ? "true" : "false", hub_lan_guest_status(), gtot, gon);
    }

    boiler_t bo = decode_boiler(f06, n06);
    if (bo.ok) {
        hex(raw, f06, n06);
        CHUNK("\"boiler\":{\"power\":%s,\"heat\":%s,\"outing\":%s,\"hotwater\":%s,\"preheat\":%s,\"heat_mode\":\"%s\",",
              bo.power ? "true" : "false", bo.heat ? "true" : "false", bo.outing ? "true" : "false",
              bo.hotwater ? "true" : "false", bo.preheat ? "true" : "false", bo.indoor ? "indoor" : "ondol");
        CHUNK("\"heat_burn\":%s,\"hw_burn\":%s,", bo.heat_burn ? "true" : "false", bo.hw_burn ? "true" : "false");
        CHUNK("\"heat_set\":%d,\"ondol_set\":%d,\"room_set\":%d,\"hw_set\":%d,\"raw\":\"%s\",",
              bo.indoor ? bo.room_set : bo.ondol_set, bo.ondol_set, bo.room_set, bo.hw_set, raw);
        hub_stg_t sg;                 /* 보일러가 1시간마다 올리는 사용량 누계(정수) — 받기 전이면 null */
        if (hub_rinnai_stg(&sg)) {
            CHUNK("\"usage\":{\"age_s\":%lld,\"heat_gas_m3\":%u,\"hw_gas_m3\":%u,\"heat_burn_h\":%u,\"hw_burn_h\":%u}},",
                  (long long)((esp_timer_get_time() / 1000 - sg.up_ms) / 1000), sg.heat_gas_m3, sg.hw_gas_m3,
                  sg.heat_burn_h, sg.hw_burn_h);
        } else {
            CHUNK("\"usage\":null},");
        }
    } else {
        CHUNK("\"boiler\":null,");
    }

    if (n09 >= 50) {
        hex(raw, f09, n09);
        CHUNK("\"rooms\":{\"raw\":\"%s\",\"zones\":[", raw);
        for (int i = 0, k = 0; i < NZONES; i++) {
            if (zone_present(f09, n09, i)) {
                CHUNK("%s" ZONE_JSON, k++ ? "," : "", ZONE_ARGS(f09, i));
            }
        }
        CHUNK("]},");
    } else {
        CHUNK("\"rooms\":null,");
    }

    /* 여러 대 전체: boilers[]·thermostats[] (각 기기 mac 포함). 위 boiler/rooms 는 첫 기기(호환) */
    hub_dev_t dev[6];
    int nd = hub_rinnai_snapshot(dev, 6);
    CHUNK("\"boilers\":[");
    int nb = 0;
    for (int i = 0; i < nd; i++) {
        if (dev[i].kind != HUB_KIND_F06) {
            continue;
        }
        boiler_t b = decode_boiler(dev[i].blob, dev[i].n);
        if (!b.ok) {
            continue;
        }
        hex(raw, dev[i].blob, dev[i].n);
        CHUNK("%s{\"mac\":\"%02x%02x%02x\",\"linked\":%s,\"power\":%s,\"heat\":%s,\"outing\":%s,\"hotwater\":%s,\"preheat\":%s,",
              nb++ ? "," : "", dev[i].mac3[0], dev[i].mac3[1], dev[i].mac3[2], dev[i].linked ? "true" : "false",
              b.power ? "true" : "false", b.heat ? "true" : "false", b.outing ? "true" : "false",
              b.hotwater ? "true" : "false", b.preheat ? "true" : "false");
        CHUNK("\"heat_mode\":\"%s\",\"heat_set\":%d,\"hw_set\":%d,\"raw\":\"%s\",", b.indoor ? "indoor" : "ondol",
              b.indoor ? b.room_set : b.ondol_set, b.hw_set, raw);
        CHUNK("\"seq\":%lu,\"chg_seq\":%lu,\"chg_up_ms\":%llu,", (unsigned long)dev[i].seq, (unsigned long)dev[i].chg_seq,
              (unsigned long long)dev[i].chg_ms);
        model_json(req, &dev[i]);
        CHUNK("}");
    }
    CHUNK("],\"thermostats\":[");
    int nt = 0;
    hub_dev_t rem[4];
    int nr = hub_upstream_snapshot(rem, 4);        /* 상류 중계 중인 1층 서머스탯 */
    for (int src = 0; src < 2; src++) {
        hub_dev_t *arr = src ? rem : dev;
        int cnt = src ? nr : nd;
        for (int i = 0; i < cnt; i++) {
            if (arr[i].kind != HUB_KIND_F09 || arr[i].n < 50) {
                continue;
            }
            hex(raw, arr[i].blob, arr[i].n);
            CHUNK("%s{\"mac\":\"%02x%02x%02x\",\"linked\":%s,\"remote\":%s,\"raw\":\"%s\",", nt++ ? "," : "",
                  arr[i].mac3[0], arr[i].mac3[1], arr[i].mac3[2], arr[i].linked ? "true" : "false",
                  src ? "true" : "false", raw);
            label_json(req, HUB_KIND_F09, arr[i].mac3);
            model_json(req, &arr[i]);
            CHUNK(",");
            CHUNK("\"seq\":%lu,\"chg_seq\":%lu,\"chg_up_ms\":%llu,\"zones\":[", (unsigned long)arr[i].seq,
                  (unsigned long)arr[i].chg_seq, (unsigned long long)arr[i].chg_ms);
            for (int z = 0, k = 0; z < NZONES; z++) {
                if (zone_present(arr[i].blob, arr[i].n, z)) {
                    CHUNK("%s" ZONE_JSON, k++ ? "," : "", ZONE_ARGS(arr[i].blob, z));
                }
            }
            CHUNK("]}");
        }
    }
    CHUNK("],");

    float tc, rh;
    int age;
    if (hub_sensors_room(&tc, &rh, &age)) {
        if (isnan(rh)) {                            /* 습도가 없는 센서(MPL3115A2 등) */
            CHUNK("\"room_sensor\":{\"temp\":%.2f,\"hum\":null,\"age_s\":%d},", tc, age);
        } else {
            CHUNK("\"room_sensor\":{\"temp\":%.2f,\"hum\":%.1f,\"age_s\":%d},", tc, rh, age);
        }
    } else {
        CHUNK("\"room_sensor\":null,");
    }
    {
        char ri[200];
        hub_sensors_room_info(ri, sizeof(ri));
        CHUNK("\"room_plugin\":%s,", ri);
    }

    gas_view_t g;
    uint32_t step;
    if (hub_sensors_gas(&g, &step)) {
        CHUNK("\"gas\":{\"pulses\":%lu,\"step_l\":%lu,\"liters_total\":%lu,", (unsigned long)g.count, (unsigned long)step,
              (unsigned long)(g.count * step));
        if (g.reading_l != 0xFFFFFFFFu) {
            CHUNK("\"meter_m3\":%lu.%03lu,", (unsigned long)(g.reading_l / 1000), (unsigned long)(g.reading_l % 1000));
        } else {
            CHUNK("\"meter_m3\":null,");
        }
        CHUNK("\"hour_l\":%lu,\"day_l\":%lu,\"flow_lph\":%lu,\"closed_ms\":%lu,\"closed_now\":%s,", (unsigned long)g.hour_l,
              (unsigned long)g.day_l, (unsigned long)g.flow_lph, (unsigned long)g.closed_ms, g.closed_now ? "true" : "false");
        /* no_pulse: 보일러를 쓰는데 펄스가 없음(경고) · burn_s: 마지막 펄스 뒤로 보일러가 연소한 시간 */
        char bp[200];
        hub_sensors_gas_burn_json(bp, sizeof(bp));
        /* burn_per_pulse: 펄스마다 센 '직전 펄스 뒤 연소한 시간' 분포(난방만 / 온수 낌) — 경고 한도를 정하려고 */
        CHUNK("\"no_pulse\":%s,\"burn_s\":%lu,\"burn_per_pulse\":", g.no_pulse ? "true" : "false", (unsigned long)g.burn_s);
        httpd_resp_sendstr_chunk(req, bp);
        httpd_resp_sendstr_chunk(req, "}");
    } else {
        CHUNK("\"gas\":null");
    }

    /* 동작 모드: set = 정해 둔 모드, fallback = 프록시인데 클라우드에 닿지 않아 지금 단독처럼 동작 중 */
    CHUNK(",\"mode\":{\"set\":\"%s\",\"fallback\":%s,\"fallback_on\":%s,\"fallback_since\":%lu,\"fallback_why\":\"%s\"}",
          hub_proxy_mode() == HUB_MODE_STANDALONE ? "standalone" : "proxy", hub_proxy_fallback() ? "true" : "false",
          hub_proxy_fallback_on() ? "true" : "false", (unsigned long)hub_proxy_fallback_since(), hub_proxy_fallback_why());
    {
        char up[160];
        hub_proxy_up_json(up, sizeof(up));          /* 린나이 서버 주소: DNS 답 그대로, TTL 이 지나면 다시 묻는다 */
        CHUNK(",\"cloud\":%s", up);
    }
    {
        /* 명령 확인·재전송 누계(켜진 뒤)와 온수 예열 자동 끄기: preheat_off_min 0 = 무제한, preheat_on_s = 예열이 켜진 지 몇 초, boiler_mode 0 정하지 않음/1 난방수/2 온돌·실내, preheat_left_s 꺼지기까지 남은 초(-1 무제한) */
        hub_cmd_info_t ci;
        hub_rinnai_cmd_info(&ci);
        CHUNK(",\"cmd\":{\"tracked\":%lu,\"resent\":%lu,\"failed\":%lu,\"reverted\":%lu,\"pending\":%d,\"preheat_off_min\":%d,\"preheat_on_s\":%lu,"
              "\"boiler_mode\":%d,\"preheat_left_s\":%ld}",
              (unsigned long)ci.tracked, (unsigned long)ci.resent, (unsigned long)ci.failed, (unsigned long)ci.reverted,
              ci.pending, ci.preheat_off_min,
              (unsigned long)ci.preheat_on_s, ci.boil_mode,
              ci.preheat_left_s == UINT32_MAX ? -1L : (long)ci.preheat_left_s);
    }

    {
        int mode = 0, billday = 8;
        const char *region = "";
        double vcf = 0, heat = 0, rate = 0, base = 0;
        long lf = 0;
        hub_tariff_tick();
        hub_tariff_cur(&mode, &region, &billday, &vcf, &heat, &rate, &base, &lf);
        CHUNK(",\"tariff\":{\"mode\":\"%s\",\"region\":\"%s\",\"billday\":%d,", mode ? "manual" : "auto", region, billday);
        static const char *const SRCN[] = { "null", "\"meter\"", "\"boiler\"" };
        int scfg = 0, sused = hub_tariff_source(&scfg);
        /* source = 설정(auto/meter/boiler), basis = 지금 실제로 세는 기준(없으면 null), months[].source = 그 달을 센 기준 */
        CHUNK("\"source\":\"%s\",\"basis\":%s,", scfg == 1 ? "meter" : scfg == 2 ? "boiler" : "auto", SRCN[sused >= 1 && sused <= 2 ? sused : 0]);
        CHUNK("\"vcf\":%.6f,\"heat\":%.4f,\"rate\":%.6f,\"base\":%.0f,\"fetched\":%ld,\"months\":[", vcf, heat, rate, base, lf);
        int mn = hub_tariff_months();
        for (int i = 0; i < mn; i++) {
            int y, mo, st, ms = 0;
            double m3;
            uint32_t won;
            hub_tariff_month_at(i, &y, &mo, &m3, &won, &st, &ms);
            CHUNK("%s{\"year\":%d,\"month\":%d,\"m3\":%.2f,\"won\":%u,\"closed\":%s,\"final\":%s,\"source\":%s}",
                  i ? "," : "", y, mo, m3, (unsigned)won, st >= 1 ? "true" : "false", st == 2 ? "true" : "false",
                  SRCN[ms == 1 || ms == 2 ? ms : 0]);
        }
        CHUNK("]}");
    }
    {
        char zb[800];
        hub_diag_zb_json(zb, sizeof(zb));
        httpd_resp_sendstr_chunk(req, ",\"zigbee\":");
        httpd_resp_sendstr_chunk(req, zb);
    }
    httpd_resp_sendstr_chunk(req, "}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---------------------------------------------------------------- /metrics (Prometheus) */

static esp_err_t metrics_get(httpd_req_t *req)
{
    if (!api_or_dbg(req)) {
        return ESP_OK;
    }
    hub_dev_t dev[6];
    int nd = hub_rinnai_snapshot(dev, 6);
    char line[256], mac[8];
    httpd_resp_set_type(req, "text/plain; version=0.0.4; charset=utf-8");
#define MACSTR3(d) (snprintf(mac, sizeof(mac), "%02x%02x%02x", (d).mac3[0], (d).mac3[1], (d).mac3[2]), mac)

    CHUNK("# TYPE lynnai_uptime_seconds gauge\nlynnai_uptime_seconds{hub=\"%s\"} %lld\n", s_id,
          (long long)(esp_timer_get_time() / 1000000));
    CHUNK("# TYPE lynnai_heap_free_bytes gauge\nlynnai_heap_free_bytes %lu\n", (unsigned long)esp_get_free_heap_size());
    CHUNK("# TYPE lynnai_wifi_rssi_dbm gauge\nlynnai_wifi_rssi_dbm %d\n", rssi());

    /* 각 지표의 줄은 모여 있어야 한다(Prometheus): 지표별로 전체 기기를 돈다 */
    CHUNK("# TYPE lynnai_link_up gauge\n");
    for (int i = 0; i < nd; i++) {
        CHUNK("lynnai_link_up{device=\"%s\",mac=\"%s\"} %d\n", dev[i].kind == HUB_KIND_F06 ? "boiler" : "rooms",
              MACSTR3(dev[i]), dev[i].linked);
    }
    {
        hub_cmd_info_t ci;
        hub_rinnai_cmd_info(&ci);
        CHUNK("# TYPE lynnai_cmd_resent_total counter\nlynnai_cmd_resent_total %lu\n"
              "# TYPE lynnai_cmd_failed_total counter\nlynnai_cmd_failed_total %lu\n"
              "# TYPE lynnai_cmd_reverted_total counter\nlynnai_cmd_reverted_total %lu\n",
              (unsigned long)ci.resent, (unsigned long)ci.failed, (unsigned long)ci.reverted);
    }
    CHUNK("# TYPE lynnai_boiler_on gauge\n");
    for (int i = 0; i < nd; i++) {
        if (dev[i].kind != HUB_KIND_F06) continue;
        boiler_t bo = decode_boiler(dev[i].blob, dev[i].n);
        if (!bo.ok) continue;
        const char *m = MACSTR3(dev[i]);
        CHUNK("lynnai_boiler_on{mac=\"%s\",what=\"power\"} %d\nlynnai_boiler_on{mac=\"%s\",what=\"heat\"} %d\n", m, bo.power, m, bo.heat);
        CHUNK("lynnai_boiler_on{mac=\"%s\",what=\"heat_burn\"} %d\nlynnai_boiler_on{mac=\"%s\",what=\"hw_burn\"} %d\n", m, bo.heat_burn, m, bo.hw_burn);
        CHUNK("lynnai_boiler_on{mac=\"%s\",what=\"outing\"} %d\nlynnai_boiler_on{mac=\"%s\",what=\"hotwater\"} %d\nlynnai_boiler_on{mac=\"%s\",what=\"preheat\"} %d\n",
              m, bo.outing, m, bo.hotwater, m, bo.preheat);
    }
    {
        hub_stg_t sg;                 /* 보일러 자체 누계(stg, 1시간마다·정수). 전원이 끊겨도 보일러 쪽 값은 유지된다 */
        if (hub_rinnai_stg(&sg)) {
            char m[7];
            snprintf(m, sizeof(m), "%02x%02x%02x", sg.mac3[0], sg.mac3[1], sg.mac3[2]);
            CHUNK("# TYPE lynnai_boiler_gas_cubic_meters counter\n"
                  "lynnai_boiler_gas_cubic_meters{mac=\"%s\",what=\"heat\"} %u\nlynnai_boiler_gas_cubic_meters{mac=\"%s\",what=\"hotwater\"} %u\n",
                  m, sg.heat_gas_m3, m, sg.hw_gas_m3);
            CHUNK("# TYPE lynnai_boiler_burn_hours counter\n"
                  "lynnai_boiler_burn_hours{mac=\"%s\",what=\"heat\"} %u\nlynnai_boiler_burn_hours{mac=\"%s\",what=\"hotwater\"} %u\n",
                  m, sg.heat_burn_h, m, sg.hw_burn_h);
            CHUNK("# TYPE lynnai_boiler_usage_age_seconds gauge\nlynnai_boiler_usage_age_seconds %lld\n",
                  (long long)((esp_timer_get_time() / 1000 - sg.up_ms) / 1000));
        }
    }
    CHUNK("# TYPE lynnai_boiler_setpoint_celsius gauge\n");
    for (int i = 0; i < nd; i++) {
        if (dev[i].kind != HUB_KIND_F06) continue;
        boiler_t bo = decode_boiler(dev[i].blob, dev[i].n);
        if (!bo.ok) continue;
        const char *m = MACSTR3(dev[i]);
        CHUNK("lynnai_boiler_setpoint_celsius{mac=\"%s\",what=\"heat\",mode=\"%s\"} %d\nlynnai_boiler_setpoint_celsius{mac=\"%s\",what=\"hotwater\"} %d\n",
              m, bo.indoor ? "indoor" : "ondol", bo.indoor ? bo.room_set : bo.ondol_set, m, bo.hw_set);
    }
    const char *const zmet[4] = { "temperature_celsius", "setpoint_celsius", "on", "heating" };
    for (int mi = 0; mi < 4; mi++) {
        CHUNK("# TYPE lynnai_zone_%s gauge\n", zmet[mi]);
        for (int i = 0; i < nd; i++) {
            if (dev[i].kind != HUB_KIND_F09 || dev[i].n < 50) continue;
            const char *m = MACSTR3(dev[i]);
            for (int z = 0; z < NZONES; z++) {
                if (!zone_present(dev[i].blob, dev[i].n, z)) {
                    continue;
                }
                uint8_t st = dev[i].blob[ZONES[z].stat];
                int v = mi == 0 ? dev[i].blob[ZONES[z].cur] : mi == 1 ? dev[i].blob[ZONES[z].set]
                        : mi == 2 ? ((st & 0x02) != 0) : ((st & 0x40) != 0);
                CHUNK("lynnai_zone_%s{mac=\"%s\",zone=\"%s\",name=\"%s\"} %d\n", zmet[mi], m, ZONES[z].id, ZONES[z].name, v);
            }
        }
    }
#undef MACSTR3
    CHUNK("# TYPE lynnai_sensor_fault gauge\n");
    if (hub_plugin_enabled("room")) {
        CHUNK("lynnai_sensor_fault{sensor=\"room\"} %d\n", (hub_sensors_faults() & PLUG_ROOM) != 0);
    }
    if (hub_plugin_enabled("gas")) {
        CHUNK("lynnai_sensor_fault{sensor=\"gas\"} %d\n", (hub_sensors_faults() & PLUG_GAS) != 0);
        gas_view_t gv;
        uint32_t st;
        if (hub_sensors_gas(&gv, &st)) {
            CHUNK("# TYPE lynnai_gas_burn_no_pulse_seconds gauge\nlynnai_gas_burn_no_pulse_seconds %lu\n", (unsigned long)gv.burn_s);
        }
    }
    float tc, rh;
    int age;
    if (hub_sensors_room(&tc, &rh, &age) && age < 600) {
        CHUNK("# TYPE lynnai_room_temperature_celsius gauge\nlynnai_room_temperature_celsius %.2f\n", tc);
        if (!isnan(rh)) {
            CHUNK("# TYPE lynnai_room_humidity_percent gauge\nlynnai_room_humidity_percent %.1f\n", rh);
        }
    }
    gas_view_t g;
    uint32_t step;
    if (hub_sensors_gas(&g, &step)) {
        CHUNK("# TYPE lynnai_gas_pulses_total counter\nlynnai_gas_pulses_total %lu\n", (unsigned long)g.count);
        CHUNK("# TYPE lynnai_gas_liters_total counter\nlynnai_gas_liters_total %lu\n", (unsigned long)(g.count * step));
        CHUNK("# TYPE lynnai_gas_flow_liters_per_hour gauge\nlynnai_gas_flow_liters_per_hour %lu\n", (unsigned long)g.flow_lph);
        if (g.reading_l != 0xFFFFFFFFu) {
            CHUNK("# TYPE lynnai_gas_meter_cubic_meters gauge\nlynnai_gas_meter_cubic_meters %lu.%03lu\n",
                  (unsigned long)(g.reading_l / 1000), (unsigned long)(g.reading_l % 1000));
        }
    }
    {
        double vcf, heat, rate, base;
        hub_tariff_tick();
        hub_tariff_cur(NULL, NULL, NULL, &vcf, &heat, &rate, &base, NULL);
        CHUNK("# TYPE lynnai_gas_tariff gauge\n");
        CHUNK("lynnai_gas_tariff{what=\"vcf\"} %.6f\nlynnai_gas_tariff{what=\"heat\"} %.4f\n", vcf, heat);
        CHUNK("lynnai_gas_tariff{what=\"rate\"} %.6f\nlynnai_gas_tariff{what=\"base\"} %.0f\n", rate, base);
        int mn = hub_tariff_months();
        if (mn > 0) {
            CHUNK("# TYPE lynnai_gas_month_cost_won gauge\n");
            for (int i = 0; i < mn; i++) {
                int y, mo, cl;
                double m3;
                uint32_t won;
                hub_tariff_month_at(i, &y, &mo, &m3, &won, &cl, NULL);
                CHUNK("lynnai_gas_month_cost_won{month=\"%04d-%02d\"} %u\n", y, mo, (unsigned)won);
            }
            CHUNK("# TYPE lynnai_gas_month_cubic_meters gauge\n");
            for (int i = 0; i < mn; i++) {
                int y, mo, cl;
                double m3;
                uint32_t won;
                hub_tariff_month_at(i, &y, &mo, &m3, &won, &cl, NULL);
                CHUNK("lynnai_gas_month_cubic_meters{month=\"%04d-%02d\"} %.2f\n", y, mo, m3);
            }
        }
    }
    {
        hub_zb_diag_t z;
        hub_diag_zb_get(&z);
        CHUNK("# TYPE lynnai_zigbee_up gauge\nlynnai_zigbee_up %d\n", z.ready && z.joined ? 1 : 0);
        CHUNK("# TYPE lynnai_zigbee_ready gauge\nlynnai_zigbee_ready %d\n", z.ready ? 1 : 0);
        CHUNK("# TYPE lynnai_zigbee_channel gauge\nlynnai_zigbee_channel %u\n", z.channel);
        if (z.ready && z.parent_rssi != 127) {
            CHUNK("# TYPE lynnai_zigbee_parent_rssi_dbm gauge\nlynnai_zigbee_parent_rssi_dbm %d\n", z.parent_rssi);
            CHUNK("# TYPE lynnai_zigbee_parent_lqi gauge\nlynnai_zigbee_parent_lqi %d\n", z.parent_lqi);
        }
        CHUNK("# TYPE lynnai_zigbee_neighbors gauge\nlynnai_zigbee_neighbors %d\n", z.nbr_count);
        uint32_t st = 0, sh = 0;
        int sr = 0, se = 0;
        hub_bbox_scan_stats(&st, &sh, &sr, &se);    /* 주기 스캔(비콘 수신률 추적, /api/v1/zbscan?auto=) */
        if (st) {
            CHUNK("# TYPE lynnai_zigbee_scan_total counter\nlynnai_zigbee_scan_total %lu\n", (unsigned long)st);
            CHUNK("# TYPE lynnai_zigbee_scan_hit_total counter\nlynnai_zigbee_scan_hit_total %lu\n", (unsigned long)sh);
            CHUNK("# TYPE lynnai_zigbee_scan_beacon_rssi_dbm gauge\nlynnai_zigbee_scan_beacon_rssi_dbm %d\n", sr);
            CHUNK("# TYPE lynnai_zigbee_scan_noise_dbm gauge\nlynnai_zigbee_scan_noise_dbm %d\n", se);
        }
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---------------------------------------------------------------- 시작 */

/* GET /api/v1/trace?since=<seq>&max=<n> — 기기↔허브↔클라우드 메시지 기록(hub_trace). 읽기 API 와 같은 인증 */
static esp_err_t trace_get(httpd_req_t *req)
{
    if (!api_or_dbg(req)) {
        return ESP_OK;
    }
    return hub_trace_http(req);
}

/* GET /api/v1/bootlog — 부팅 기록(이번·직전 부팅 로그, 부팅 이력). 읽기 API 와 같은 인증 */
static esp_err_t bootlog_get(httpd_req_t *req)
{
    if (!api_or_dbg(req)) {
        return ESP_OK;
    }
    return hub_bbox_http(req);
}

/* GET /api/v1/roomtest — 온습도 센서 시험: 세 종류를 한 번씩 읽어 본다(선을 나란히 이었을 때 전부 읽히는지) */
static esp_err_t roomtest_get(httpd_req_t *req)
{
    if (!api_or_dbg(req)) {
        return ESP_OK;
    }
    char out[360];
    hub_sensors_room_test(out, sizeof(out));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

/* GET /api/v1/zbscan — Zigbee 스캔 시험(hub_bbox). 라디오를 잠깐 쓰고 pause=1 이면 Wi-Fi 도 멈추므로 읽기 API 와 같은 인증 */
static esp_err_t zbscan_get(httpd_req_t *req)
{
    if (!api_or_dbg(req)) {
        return ESP_OK;
    }
    return hub_bbox_scan_http(req);
}

void hub_api_register(httpd_handle_t srv)
{
    const httpd_uri_t zs = { .uri = "/api/v1/zbscan", .method = HTTP_GET, .handler = zbscan_get };
    const httpd_uri_t rt = { .uri = "/api/v1/roomtest", .method = HTTP_GET, .handler = roomtest_get };
    httpd_register_uri_handler(srv, &rt);
    httpd_register_uri_handler(srv, &zs);
    const httpd_uri_t bl = { .uri = "/api/v1/bootlog", .method = HTTP_GET, .handler = bootlog_get };
    httpd_register_uri_handler(srv, &bl);
    const httpd_uri_t tr = { .uri = "/api/v1/trace", .method = HTTP_GET, .handler = trace_get };
    httpd_register_uri_handler(srv, &tr);
    const httpd_uri_t st = { .uri = "/api/v1/state", .method = HTTP_GET, .handler = state_get };
    const httpd_uri_t me = { .uri = "/metrics", .method = HTTP_GET, .handler = metrics_get };
    httpd_register_uri_handler(srv, &st);
    httpd_register_uri_handler(srv, &me);
}

void hub_api_start(const char *device_id)
{
    s_id = device_id;
    token_load();
    ESP_LOGI(TAG, "서버 API: 토큰 %s", s_token[0] ? "있음" : "없음(LAN 에 열림)");
    setenv("TZ", "KST-9", 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 시작 실패");
    }
}
