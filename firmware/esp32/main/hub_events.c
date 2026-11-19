/*
 * hub_events.c -- 연결 품질 이벤트 링 버퍼 + 조회 API.
 *
 * 허브는 시계를 몰라도 된다: 각 이벤트에 부팅 후 ms(up)를 붙이고, 응답에 현재 up 과 부팅 ID 를 같이 준다.
 * NAS 기록기가 "받은 시각 - (지금 up - 이벤트 up)" 으로 실제 시각을 계산한다.
 *   GET /api/events?since=<seq>  → {"boot":..,"up":..,"next":..,"lost":..,"ev":[[seq,up,type,mac,a,b],...]}
 *   GET /api/stats               → 종류별 누적 횟수, 힙, 업타임
 * 비밀값은 없다(MAC·시간·사유 코드뿐). 집 Wi-Fi 쪽에서 읽는다.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hub_events.h"
#include "hub_zigbee.h"
#include "esp_zigbee.h"
#include "ezbee/bdb.h"
#include "hub_api.h"
#include "hub_store.h"
#include "hub_led.h"

#define RING 256

typedef struct {
    uint32_t seq;
    uint32_t up_ms;
    int32_t a, b;
    uint8_t type;
    uint8_t mac[6];
} ev_t;

static ev_t s_ring[RING];
static uint32_t s_next;                 /* 다음에 쓸 seq (0 부터) */
static uint32_t s_count[EV__COUNT];
static uint32_t s_boot_id;
static SemaphoreHandle_t s_lock;

static const char *const NAMES[EV__COUNT] = {
    "boot", "ap_join", "ap_leave", "sta_up", "sta_down", "link_open", "link_dev_tls", "link_cloud_tls",
    "link_up", "link_close", "link_reject", "bootstrap", "zb_join_fail", "zb_joined", "zb_leave", "onboard",
    "gas_pulse", "store_bad",
};

static void ensure_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_boot_id = esp_random();
    }
}

void hub_event(hub_ev_t type, const uint8_t *mac, int32_t a, int32_t b)
{
    ensure_init();
    if (type >= EV__COUNT) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ev_t *e = &s_ring[s_next % RING];
    e->seq = s_next++;
    e->up_ms = (uint32_t)(esp_timer_get_time() / 1000);
    e->type = type;
    e->a = a;
    e->b = b;
    if (mac) {
        memcpy(e->mac, mac, 6);
    } else {
        memset(e->mac, 0, 6);
    }
    s_count[type]++;
    xSemaphoreGive(s_lock);
}

static esp_err_t events_get(httpd_req_t *req)
{
    ensure_init();
    if (!hub_api_authorized(req)) {
        return ESP_OK;
    }
    char q[48], v[16];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
        since = strtoul(v, NULL, 10);
    }
    httpd_resp_set_type(req, "application/json");
    char line[112];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t next = s_next;
    uint32_t oldest = next > RING ? next - RING : 0;
    uint32_t lost = 0;
    if (since > next) {                      /* 허브가 재부팅해 seq 가 되돌아간 경우: 처음부터 */
        since = 0;
    }
    if (since < oldest) {
        lost = oldest - since;
        since = oldest;
    }
    /* 한 번에 최대 128개(응답이 너무 커지지 않게). 나머지는 다음 호출에서 */
    uint32_t end = next - since > 128 ? since + 128 : next;
    ev_t *copy = malloc(sizeof(ev_t) * (end - since + 1));
    uint32_t n = 0;
    if (copy) {
        for (uint32_t s = since; s < end; s++) {
            copy[n++] = s_ring[s % RING];
        }
    }
    xSemaphoreGive(s_lock);
    if (!copy) {
        return httpd_resp_send_500(req);
    }

    snprintf(line, sizeof(line), "{\"boot\":%lu,\"up\":%lu,\"next\":%lu,\"lost\":%lu,\"ev\":[",
             (unsigned long)s_boot_id, (unsigned long)(esp_timer_get_time() / 1000), (unsigned long)end,
             (unsigned long)lost);
    httpd_resp_sendstr_chunk(req, line);
    for (uint32_t i = 0; i < n; i++) {
        const ev_t *e = &copy[i];
        snprintf(line, sizeof(line), "%s[%lu,%lu,\"%s\",\"%02x:%02x:%02x:%02x:%02x:%02x\",%ld,%ld]",
                 i ? "," : "", (unsigned long)e->seq, (unsigned long)e->up_ms, NAMES[e->type], e->mac[0],
                 e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], (long)e->a, (long)e->b);
        httpd_resp_sendstr_chunk(req, line);
    }
    free(copy);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t stats_get(httpd_req_t *req)
{
    ensure_init();
    if (!hub_api_authorized(req)) {
        return ESP_OK;
    }
    char buf[160];
    extern bool g_zb_nvs_was_reset;          /* hub_main: 이번 부팅에 zb_nvs 를 지우고 새로 만들었나(가입 정보 날아감) */
    httpd_resp_set_type(req, "application/json");
    esp_zigbee_lock_acquire(portMAX_DELAY);
    bool zb = ezb_bdb_dev_joined();         /* 실제 가입 여부(zb_joined 카운트는 '이번 부팅 steering 성공 수'일 뿐) */
    bool fresh = ezb_bdb_is_factory_new();
    esp_zigbee_lock_release();
    snprintf(buf, sizeof(buf), "{\"boot\":%lu,\"up\":%lu,\"zb_joined_now\":%s,\"zb_factory_new\":%s,\"zb_nvs_reset\":%s,"
             "\"heap\":%lu,\"heap_min\":%lu,",
             (unsigned long)s_boot_id, (unsigned long)(esp_timer_get_time() / 1000), zb ? "true" : "false",
             fresh ? "true" : "false", g_zb_nvs_was_reset ? "true" : "false",
             (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
    httpd_resp_sendstr_chunk(req, buf);
    {
        char errs[4 * 60 + 8];                  /* 저장 데이터 오류(hub_store): 건수 + 최근 4개 */
        hub_store_errors_json(errs, sizeof(errs));
        snprintf(buf, sizeof(buf), "\"store_bad\":%d,\"led_alarm\":%s,\"store_errors\":", hub_store_error_count(),
                 hub_led_alarm_on() ? "true" : "false");
        httpd_resp_sendstr_chunk(req, buf);
        httpd_resp_sendstr_chunk(req, errs);
        httpd_resp_sendstr_chunk(req, ",\"count\":{");
    }
    for (int i = 0; i < EV__COUNT; i++) {
        snprintf(buf, sizeof(buf), "%s\"%s\":%lu", i ? "," : "", NAMES[i], (unsigned long)s_count[i]);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "}}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t zb_join_get(httpd_req_t *req)
{
    if (!hub_api_authorized(req)) {
        return ESP_OK;
    }
    hub_zigbee_join_now();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"note\":\"3분간 10초마다 가입 시도\"}");
}

void hub_events_register(httpd_handle_t srv)
{
    const httpd_uri_t zj = { .uri = "/api/zigbee/join", .method = HTTP_GET, .handler = zb_join_get };
    httpd_register_uri_handler(srv, &zj);
    const httpd_uri_t ev = { .uri = "/api/events", .method = HTTP_GET, .handler = events_get };
    const httpd_uri_t st = { .uri = "/api/stats", .method = HTTP_GET, .handler = stats_get };
    httpd_register_uri_handler(srv, &ev);
    httpd_register_uri_handler(srv, &st);
}
