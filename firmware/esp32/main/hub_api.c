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
#include "hub_sensors.h"
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
/* 온도 조절기가 리포트하는 방만(상태·현재 온도 둘 다 0 = 그 슬롯에 방 없음, 드라이버와 같은 기준) */
static bool zone_present(const uint8_t *b, int n, int z) { return ZONES[z].set < n && (b[ZONES[z].stat] || b[ZONES[z].cur]); }
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

#define CHUNK(...) do { snprintf(line, sizeof(line), __VA_ARGS__); httpd_resp_sendstr_chunk(req, line); } while (0)

static esp_err_t state_get(httpd_req_t *req)
{
    if (!hub_api_authorized(req)) {
        return ESP_OK;
    }
    uint8_t f06[64], f09[64], links;
    int n06, n09;
    legacy_snapshot(f06, &n06, f09, &n09, &links);
    char line[256], raw[132];
    long t = now_epoch();
    httpd_resp_set_type(req, "application/json");

    CHUNK("{\"v\":1,\"id\":\"%s\",\"up_s\":%lld,", s_id, (long long)(esp_timer_get_time() / 1000000));
    if (t) {
        CHUNK("\"time\":%ld,", t);
    } else {
        CHUNK("\"time\":null,");
    }
    CHUNK("\"hub\":{\"heap\":%lu,\"heap_min\":%lu,\"rssi\":%d,\"plugins\":%u,\"sensor_faults\":%u},", (unsigned long)esp_get_free_heap_size(),
          (unsigned long)esp_get_minimum_free_heap_size(), rssi(), hub_plugins_mask(), hub_sensors_faults());
    CHUNK("\"links\":{\"boiler\":%s,\"rooms\":%s},", links & 1 ? "true" : "false", links & 2 ? "true" : "false");

    boiler_t bo = decode_boiler(f06, n06);
    if (bo.ok) {
        hex(raw, f06, n06);
        CHUNK("\"boiler\":{\"power\":%s,\"heat\":%s,\"outing\":%s,\"hotwater\":%s,\"preheat\":%s,\"heat_mode\":\"%s\",",
              bo.power ? "true" : "false", bo.heat ? "true" : "false", bo.outing ? "true" : "false",
              bo.hotwater ? "true" : "false", bo.preheat ? "true" : "false", bo.indoor ? "indoor" : "ondol");
        CHUNK("\"heat_burn\":%s,\"hw_burn\":%s,", bo.heat_burn ? "true" : "false", bo.hw_burn ? "true" : "false");
        CHUNK("\"heat_set\":%d,\"ondol_set\":%d,\"room_set\":%d,\"hw_set\":%d,\"raw\":\"%s\"},",
              bo.indoor ? bo.room_set : bo.ondol_set, bo.ondol_set, bo.room_set, bo.hw_set, raw);
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
        CHUNK("\"heat_mode\":\"%s\",\"heat_set\":%d,\"hw_set\":%d,\"raw\":\"%s\"}", b.indoor ? "indoor" : "ondol",
              b.indoor ? b.room_set : b.ondol_set, b.hw_set, raw);
    }
    CHUNK("],\"thermostats\":[");
    int nt = 0;
    for (int i = 0; i < nd; i++) {
        if (dev[i].kind != HUB_KIND_F09 || dev[i].n < 50) {
            continue;
        }
        hex(raw, dev[i].blob, dev[i].n);
        CHUNK("%s{\"mac\":\"%02x%02x%02x\",\"linked\":%s,\"raw\":\"%s\",\"zones\":[", nt++ ? "," : "",
              dev[i].mac3[0], dev[i].mac3[1], dev[i].mac3[2], dev[i].linked ? "true" : "false", raw);
        for (int z = 0, k = 0; z < NZONES; z++) {
            if (zone_present(dev[i].blob, dev[i].n, z)) {
                CHUNK("%s" ZONE_JSON, k++ ? "," : "", ZONE_ARGS(dev[i].blob, z));
            }
        }
        CHUNK("]}");
    }
    CHUNK("],");

    float tc, rh;
    int age;
    if (hub_sensors_room(&tc, &rh, &age)) {
        CHUNK("\"room_sensor\":{\"temp\":%.2f,\"hum\":%.1f,\"age_s\":%d},", tc, rh, age);
    } else {
        CHUNK("\"room_sensor\":null,");
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
        CHUNK("\"hour_l\":%lu,\"day_l\":%lu,\"flow_lph\":%lu}", (unsigned long)g.hour_l, (unsigned long)g.day_l,
              (unsigned long)g.flow_lph);
    } else {
        CHUNK("\"gas\":null");
    }
    httpd_resp_sendstr_chunk(req, "}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---------------------------------------------------------------- /metrics (Prometheus) */

static esp_err_t metrics_get(httpd_req_t *req)
{
    if (!hub_api_authorized(req)) {
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
    }
    float tc, rh;
    int age;
    if (hub_sensors_room(&tc, &rh, &age) && age < 600) {
        CHUNK("# TYPE lynnai_room_temperature_celsius gauge\nlynnai_room_temperature_celsius %.2f\n", tc);
        CHUNK("# TYPE lynnai_room_humidity_percent gauge\nlynnai_room_humidity_percent %.1f\n", rh);
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
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* ---------------------------------------------------------------- 시작 */

void hub_api_register(httpd_handle_t srv)
{
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
