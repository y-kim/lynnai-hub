/*
 * hub_tariff.c -- 도시가스 요금 요소를 받아(자동) 또는 직접 넣어(수동) 드라이버로 중계하고,
 *                 허브가 쥔 가스 사용량으로 검침일 기준 월별 청구액을 쌓는다. hub_tariff.h 참고.
 *
 *   요금(보정·열량·단가): 자동=gas.yangsu.kim/api/bill(구역별), 수동=직접 입력.
 *   기본료·검침일: 내 정보(항상 사용자 값).
 *   구역 목록: /api/regions 를 받아 관리 화면 datalist(검색 선택)로.
 *   사용량 기준: 가스 계량기(플러그인 펄스) 또는 보일러 자체 누계(stg — 약 1시간마다, 정수 ㎥, 난방+온수).
 *            설정 = 자동(계량기가 있으면 계량기, 없으면 보일러) / 계량기 / 보일러. 기간 중에 기준이 바뀌면 그때까지
 *            센 양은 두고 새 기준에서 이어 센다. 보일러 카드(드라이버·HA)에는 CMD 0x0f 로 이번 기간 값을 보낸다.
 *   월 마감: 청구월 N = (N-1)월 검침일 다음 날 ~ N월 검침일(검침일 당일까지가 그 달 몫). 검침일을 "말일"(31)로 두면
 *            N월 1일 ~ N월 말일. 정확 열량(사용기간 가중평균)이 확정되면 그 청구액으로,
 *            아니면 검침일+3일 뒤 최신 월 열량 청구액으로 등록(/api/bill 의 청구액 = 청구서 재현).
 */
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "lwip/dns.h"
#include "nvs.h"
#include "esp_system.h"
#include "hub_uplink.h"
#include "hub_proxy.h"
#include "hub_sensors.h"
#include "hub_plugins.h"
#include "hub_rinnai.h"
#include "hub_tariff.h"
#include "hub_wifi.h"

static const char *TAG = "HUB_TARIFF";

extern const char gas_ca_pem_start[] asm("_binary_gas_ca_pem_start");
extern const char gas_ca_pem_end[]   asm("_binary_gas_ca_pem_end");

#define NS            "gas"
#define FETCH_OK_S    (12 * 3600)
#define FETCH_RETRY_S 15
/* 부팅 직후엔 보일러·각방 연결의 TLS 핸드셰이크가 몰려 힙이 빠듯하다 — 그게 끝난 뒤에 조회를 시작한다 */
#define FIRST_DELAY_S 40
#define TLS_MIN_FREE  44000      /* HTTPS 한 번에 40KB 남짓 쓴다. 이만큼 남아 있을 때만 시작 */
#define WAKE_S        600
#define FIN_THROTTLE_S 3600          /* 마감 재시도는 한 시간에 한 번 */
#define FIN_GRACE_D   3              /* 정확 열량이 없으면 검침일+이 일수 뒤 최신 월값으로 마감 */
#define BUF_MAX       2048
#define REG_RAW       3400
#define REG_OPTS      6000
#define CLOCK_OK(t)   ((t) > 1704067200L)

#define MODE_AUTO 0
#define MODE_MANUAL 1

/* 사용량 기준(설정 tsrc). 월 기록의 src 에는 실제로 센 기준(1·2)이 들어간다 */
#define SRC_AUTO   0
#define SRC_METER  1
#define SRC_BOILER 2
static uint8_t s_src_cfg = SRC_AUTO;

#define DEF_VCF_E6  993900u
#define DEF_HEAT_E6 42576000u
#define DEF_RATE_E6 22622600u
#define DEF_BASE    1250u

static char     s_region[12] = "SD07";
static uint8_t  s_mode     = MODE_AUTO;
#define BILLDAY_LAST  32             /* 검침일 "말일": 그 달의 마지막 날(28~31일) */
static uint8_t  s_billday  = 8;      /* 1~31(그 날이 없는 달에는 마지막 날) 또는 BILLDAY_LAST */
static uint32_t s_man_vcf_e6  = DEF_VCF_E6;
static uint32_t s_man_heat_e6 = DEF_HEAT_E6;
static uint32_t s_man_rate_e6 = DEF_RATE_E6;
static uint32_t s_man_base    = DEF_BASE;        /* 기본료 = 내 정보(항상 이 값 사용) */

static uint32_t s_fet_vcf_e6, s_fet_heat_e6, s_fet_rate_e6;   /* 자동: 마지막 조회값(보정·열량·단가) */
static bool     s_fet_valid;
static long     s_fet_epoch;
static volatile bool s_fet_want;                 /* 새로고침: 다음 바퀴에 꼭 다시 받는다(받아 둔 값은 그동안 그대로 쓴다) */
static volatile bool s_fet_busy;                 /* 받는 중(요청부터 그 시도가 끝날 때까지) — 화면 안내용 */
static char     s_fet_err[40];                   /* 마지막 시도가 실패한 까닭(성공하면 빈 문자열) */
static uint8_t  s_fet_fails;                     /* 잇따라 실패한 횟수(다시 받기 간격을 늘린다) */
static uint32_t s_fet_retry_s;                   /* 실패 뒤 다음 시도까지(0 = 예정 없음) */
static char     s_get_err[40];                   /* http_get 이 실패한 까닭 */
static char     s_fet_regname[40];               /* "지역" */
static char     s_fet_heatbasis[72];             /* "열량기준" (측정 기간/출처) */

static char     s_reg_opts[REG_OPTS];            /* 구역 datalist <option>… (한 번 받아 둠) */
static bool     s_regs_got;

static TaskHandle_t s_task;
static char s_buf[BUF_MAX];
static uint32_t s_relay_sig = 0xFFFFFFFFu;
static volatile bool s_bill_want;            /* 설정을 바꿨다: 태스크가 보일러 카드용 0x0f 를 곧 다시 보낸다 */
static int64_t  s_fin_last_us;

/* ---- 월별 기록 ---- */
#define MON_MAX 13
typedef struct {
    uint16_t ym;        /* 청구월(연*12 + 월-1). 기간 = (월-1) 검침일 다음 날 ~ 월 검침일 */
    uint8_t  closed;    /* 1 = 기간 끝남(마감 대기/완료) */
    uint8_t  final;     /* 1 = 정확 열량으로 확정 등록됨 */
    uint8_t  src;       /* 지금(마지막으로) 세는 기준 SRC_METER / SRC_BOILER */
    uint8_t  mac3[3];   /* 보일러 기준이면 그 보일러 MAC끝3 */
    uint32_t start_l;   /* 기간 시작(또는 기준이 바뀐 때) 그 기준의 누적 리터 */
    uint32_t carry_l;   /* 기준이 바뀌기 전까지 센 리터 */
    uint32_t use_l;     /* 사용 리터 = carry_l + (지금 누적 - start_l) */
    uint32_t won;       /* 청구액 원 */
} tmon_t;
typedef struct { uint8_t n; tmon_t m[MON_MAX]; } mon_blob_t;
/* v0.9 까지의 기록(tmon2) — 계량기로만 셌다. 처음 읽을 때 옮긴다 */
typedef struct {
    uint16_t ym;
    uint32_t start_l, use_l, won;
    uint8_t  closed, final;
} tmon2_t;
typedef struct { uint8_t n; tmon2_t m[MON_MAX]; } mon2_blob_t;
static tmon_t s_mon[MON_MAX];
static int    s_mon_n;
static SemaphoreHandle_t s_lock;

/* ---------------------------------------------------------------- 저장 */
static void save_cfg(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "region", s_region);
    nvs_set_u8(h, "tmode", s_mode);
    nvs_set_u8(h, "tbillday", s_billday);
    nvs_set_u32(h, "tvcf", s_man_vcf_e6);
    nvs_set_u32(h, "theat", s_man_heat_e6);
    nvs_set_u32(h, "trate", s_man_rate_e6);
    nvs_set_u32(h, "tbase", s_man_base);
    nvs_set_u8(h, "tsrc", s_src_cfg);
    nvs_commit(h);
    nvs_close(h);
}

static void load_cfg(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_region);
    nvs_get_str(h, "region", s_region, &n);
    nvs_get_u8(h, "tmode", &s_mode);
    nvs_get_u8(h, "tbillday", &s_billday);
    nvs_get_u32(h, "tvcf", &s_man_vcf_e6);
    nvs_get_u32(h, "theat", &s_man_heat_e6);
    nvs_get_u32(h, "trate", &s_man_rate_e6);
    nvs_get_u32(h, "tbase", &s_man_base);
    nvs_get_u8(h, "tsrc", &s_src_cfg);
    nvs_close(h);
    if (s_billday < 1 || s_billday > BILLDAY_LAST) s_billday = 8;
    if (s_src_cfg > SRC_BOILER) s_src_cfg = SRC_AUTO;
}

static void mon_save(void)
{
    mon_blob_t *bl = calloc(1, sizeof(mon_blob_t));      /* httpd 핸들러에서도 불린다 — 힙에 */
    if (!bl) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bl->n = (uint8_t)s_mon_n;
    memcpy(bl->m, s_mon, sizeof(s_mon));
    xSemaphoreGive(s_lock);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "tmon3", bl, sizeof(*bl));
        nvs_commit(h);
        nvs_close(h);
    }
    free(bl);
}

static void mon_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    mon_blob_t *bl = calloc(1, sizeof(mon_blob_t));      /* 둘 다 300바이트 남짓 — 스택 대신 힙 */
    mon2_blob_t *old = calloc(1, sizeof(mon2_blob_t));
    size_t n = sizeof(*bl), n2 = sizeof(*old);
    bool moved = false;
    if (bl && nvs_get_blob(h, "tmon3", bl, &n) == ESP_OK && n == sizeof(*bl) && bl->n <= MON_MAX) {
        s_mon_n = bl->n;
        memcpy(s_mon, bl->m, sizeof(s_mon));
    } else if (old && nvs_get_blob(h, "tmon2", old, &n2) == ESP_OK && n2 == sizeof(*old) && old->n <= MON_MAX) {
        s_mon_n = old->n;                                 /* 옛 기록: 계량기 기준으로 옮긴다 */
        memset(s_mon, 0, sizeof(s_mon));
        for (int i = 0; i < s_mon_n; i++) {
            s_mon[i].ym = old->m[i].ym;
            s_mon[i].closed = old->m[i].closed;
            s_mon[i].final = old->m[i].final;
            s_mon[i].src = SRC_METER;
            s_mon[i].start_l = old->m[i].start_l;
            s_mon[i].use_l = old->m[i].use_l;
            s_mon[i].won = old->m[i].won;
        }
        moved = true;
    }
    nvs_close(h);
    free(bl);
    free(old);
    if (moved) {
        ESP_LOGI(TAG, "월별 기록 %d개를 새 형식(tmon3)으로 옮김", s_mon_n);
        mon_save();
    }
}

/* ---------------------------------------------------------------- 요금 */
/* 유효 요금(e6): 보정·열량·단가는 수동/자동, 기본료는 항상 내 정보. */
static void eff_e6(uint32_t *vcf, uint32_t *heat, uint32_t *rate, uint32_t *base)
{
    if (s_mode == MODE_MANUAL || !s_fet_valid) {
        *vcf = s_man_vcf_e6; *heat = s_man_heat_e6; *rate = s_man_rate_e6;
    } else {
        *vcf = s_fet_vcf_e6; *heat = s_fet_heat_e6; *rate = s_fet_rate_e6;
    }
    *base = s_man_base;
}

static uint32_t won_of(double m3)
{
    uint32_t v, h, r, b;
    eff_e6(&v, &h, &r, &b);
    double fee  = m3 * (v / 1e6) * (h / 1e6) * (r / 1e6);
    double bill = (b + fee) * 1.1;
    if (bill < 0) bill = 0;
    return (uint32_t)llround(bill);
}

static void relay_current(void)
{
    if (!hub_plugin_enabled("gas")) {                /* 가스 계량이 없으면 요금도 끈다(드라이버에 0x09 안 보냄) */
        return;
    }
    uint32_t p[5];                                   /* 다섯째 = 검침일(1~31, 32 = 말일). 옛 드라이버는 앞의 넷만 읽는다 */
    eff_e6(&p[0], &p[1], &p[2], &p[3]);
    p[4] = s_billday;
    uint32_t sig = p[0] ^ (p[1] << 1) ^ (p[2] << 2) ^ (p[3] << 3) ^ (p[4] << 24);
    if (sig == s_relay_sig) return;
    hub_uplink_send(HUB_RK_CMD_TARIFF, (uint8_t *)p, sizeof(p));
    s_relay_sig = sig;
    ESP_LOGI(TAG, "요금 중계(%s): 보정 %.4f 열량 %.3f 단가 %.4f 기본 %u → 드라이버",
             s_mode == MODE_MANUAL ? "수동" : "자동",
             p[0] / 1e6, p[1] / 1e6, p[2] / 1e6, (unsigned)p[3]);
}

/* ---------------------------------------------------------------- HTTP(핀한 CA) */
typedef struct { char *buf; int cap; int len; } rx_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        rx_t *rx = e->user_data;
        int n = e->data_len;
        if (rx->len + n > rx->cap - 1) n = rx->cap - 1 - rx->len;
        if (n > 0) { memcpy(rx->buf + rx->len, e->data, n); rx->len += n; rx->buf[rx->len] = 0; }
    }
    return ESP_OK;
}

/* 전역 리졸버가 허브 AP 의 캡티브 DNS(클라우드 이름만 답함)로 잡혀 있을 수 있어, 조회 직전 집 쪽 DNS 로 맞춘다.
 * 공용 DNS(1.1.1.1 등)로 바꾸지 않는다 — 외부 DNS 를 막은 집에서도 되고, 집 DNS 의 설정(린나이 주소 고정 등)도
 * 그대로 따른다(프록시의 클라우드 이름 조회와 같은 후보: DHCP 로 받은 DNS → 보조 → 공유기, 허브 AP 주소는 뺀다). */
static void use_sta_dns(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) {
        return;
    }
    uint32_t ap = hub_wifi_ap_ip(), cand[3] = { 0 };
    int nc = 0;
    esp_netif_dns_info_t d;
    for (int k = 0; k < 2; k++) {
        if (esp_netif_get_dns_info(sta, k == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &d) == ESP_OK &&
            d.ip.type == ESP_IPADDR_TYPE_V4 && d.ip.u_addr.ip4.addr && d.ip.u_addr.ip4.addr != ap) {
            cand[nc++] = d.ip.u_addr.ip4.addr;
        }
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.gw.addr) {
        cand[nc++] = ip.gw.addr;                     /* 공유기(대개 DNS 중계도 함) */
    }
    for (int i = 0, slot = 0; i < nc && slot < 2; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) dup |= cand[j] == cand[i];
        if (dup) continue;
        ip_addr_t a = { 0 };
        a.type = IPADDR_TYPE_V4;
        ip4_addr_set_u32(ip_2_ip4(&a), cand[i]);
        dns_setserver(slot++, &a);
    }
}

/* 성공하면 받은 바이트 수, 실패 -1. */
/* HTTPS 를 시작해도 되는 때까지 기다린다(최대 30초): 기기 연결이 핸드셰이크 중이 아니고 힙이 충분할 때. */
static bool tls_room(void)
{
    for (int i = 0; i < 10; i++) {
        if (!hub_proxy_handshaking() && esp_get_free_heap_size() >= TLS_MIN_FREE) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    return false;
}

/* HTTPS 한 번이 힙을 얼마나 쓰는지 잰다(TLS_MIN_FREE 를 근거 있게 잡으려고): 받는 동안 10ms 마다 남은 힙의 최저를 본다 */
static volatile uint32_t s_get_low;

static void get_low_tick(void *arg)
{
    uint32_t f = esp_get_free_heap_size();
    if (f < s_get_low) {
        s_get_low = f;
    }
}

static int http_get(const char *url, char *buf, int cap)
{
    buf[0] = 0;
    if (!tls_room()) {
        ESP_LOGW(TAG, "GET 미룸(메모리 여유 %lu B) %s", (unsigned long)esp_get_free_heap_size(), url);
        strlcpy(s_get_err, "허브가 바빠서 미룸", sizeof(s_get_err));
        return -1;
    }
    use_sta_dns();
    rx_t rx = { buf, cap, 0 };
    buf[0] = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http,
        .user_data = &rx,
        .cert_pem = gas_ca_pem_start,
        .cert_len = (size_t)(gas_ca_pem_end - gas_ca_pem_start),
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };
    uint32_t before = esp_get_free_heap_size();
    s_get_low = before;
    esp_timer_handle_t tick = NULL;
    const esp_timer_create_args_t ta = { .callback = get_low_tick, .name = "get_low" };
    if (esp_timer_create(&ta, &tick) == ESP_OK) {
        esp_timer_start_periodic(tick, 10000);
    }
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_err_t err = c ? esp_http_client_perform(c) : ESP_FAIL;
    int status = c ? esp_http_client_get_status_code(c) : 0;
    if (c) {
        esp_http_client_cleanup(c);
    }
    if (tick) {
        esp_timer_stop(tick);
        esp_timer_delete(tick);
    }
    ESP_LOGI(TAG, "GET 메모리: 시작 %lu B, 받는 동안 최저 %lu B (쓴 양 %lu B)", (unsigned long)before, (unsigned long)s_get_low,
             (unsigned long)(before - s_get_low));
    if (!c) {
        strlcpy(s_get_err, "허브 메모리 부족", sizeof(s_get_err));
        return -1;
    }
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "GET 실패 %s err=%s status=%d", url, esp_err_to_name(err), status);
        if (err != ESP_OK) {
            strlcpy(s_get_err, "서버에 연결하지 못함", sizeof(s_get_err));
        } else {
            snprintf(s_get_err, sizeof(s_get_err), "서버 응답 %d", status);
        }
        return -1;
    }
    return rx.len;
}

static bool num_after(const char *body, const char *key, double *out)
{
    const char *p = strstr(body, key);
    if (!p) return false;
    p = strchr(p + strlen(key), ':');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    char *end;
    double v = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

static void str_after(const char *body, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    const char *p = strstr(body, key);
    if (!p) return;
    p = strchr(p + strlen(key), ':');
    if (!p) return;
    p = strchr(p, '"');
    if (!p) return;
    p++;
    size_t i = 0;
    for (; *p && *p != '"' && i + 1 < cap; p++) out[i++] = *p;
    out[i] = 0;
}

/* ---------------------------------------------------------------- 자동 조회 */
static bool fetch_auto(void)
{
    char url[128];
    snprintf(url, sizeof(url), "https://gas.yangsu.kim/api/bill?m3=0&region=%s", s_region);
    if (http_get(url, s_buf, BUF_MAX) < 0) {
        strlcpy(s_fet_err, s_get_err, sizeof(s_fet_err));
        return false;
    }
    double vcf, heat, rate;
    if (!num_after(s_buf, "\"보정계수\"", &vcf) ||
        !num_after(s_buf, "\"열량\"", &heat) ||
        !num_after(s_buf, "\"단가\"", &rate)) {
        ESP_LOGW(TAG, "응답 파싱 실패");
        strlcpy(s_fet_err, "서버 응답을 읽지 못함", sizeof(s_fet_err));
        return false;
    }
    if (vcf < 0.9 || vcf > 1.1 || heat < 20 || heat > 60 || rate <= 0 || rate > 100) {
        ESP_LOGW(TAG, "값 범위 벗어남 vcf=%.4f heat=%.3f rate=%.4f", vcf, heat, rate);
        strlcpy(s_fet_err, "받은 값이 범위를 벗어남", sizeof(s_fet_err));
        return false;
    }
    s_fet_vcf_e6  = (uint32_t)lround(vcf * 1e6);
    s_fet_heat_e6 = (uint32_t)lround(heat * 1e6);
    s_fet_rate_e6 = (uint32_t)lround(rate * 1e6);
    s_fet_valid   = true;
    s_fet_err[0]  = 0;
    { long nw = time(NULL); s_fet_epoch = CLOCK_OK(nw) ? nw : 0; }   /* 동기 전이면 0(업타임 값 노출 안 함) */
    str_after(s_buf, "\"지역\"", s_fet_regname, sizeof(s_fet_regname));
    str_after(s_buf, "\"열량기준\"", s_fet_heatbasis, sizeof(s_fet_heatbasis));
    ESP_LOGI(TAG, "요금 받음: 보정 %.4f 열량 %.3f(%s) 단가 %.4f", vcf, heat, s_fet_heatbasis, rate);
    return true;
}

/* /api/regions 를 받아 datalist <option> 문자열로 만들어 둔다(검색 선택용). */
static void opt_cat(char *out, size_t cap, const char *s)   /* <>&" 만 간단 이스케이프 */
{
    size_t o = strlen(out);
    for (; *s && o + 7 < cap; s++) {
        const char *r = NULL;
        switch (*s) { case '<': r="&lt;"; break; case '>': r="&gt;"; break;
                      case '&': r="&amp;"; break; case '"': r="&quot;"; break; }
        if (r) { strlcpy(out + o, r, cap - o); o += strlen(r); }
        else { out[o++] = *s; out[o] = 0; }
    }
}

static void fetch_regions(void)
{
    char *raw = malloc(REG_RAW);
    if (!raw) return;
    if (http_get("https://gas.yangsu.kim/api/regions", raw, REG_RAW) < 0) { free(raw); return; }
    s_reg_opts[0] = 0;
    const char *p = raw;
    int cnt = 0;
    while ((p = strstr(p, "\"code\"")) != NULL) {
        char code[16] = "", desc[72] = "";
        str_after(p, "\"code\"", code, sizeof(code));
        const char *d = strstr(p, "\"설명\"");
        if (d) str_after(d, "\"설명\"", desc, sizeof(desc));
        if (code[0]) {
            strlcat(s_reg_opts, "<option value=\"", REG_OPTS);
            opt_cat(s_reg_opts, REG_OPTS, code);
            strlcat(s_reg_opts, "\">", REG_OPTS);
            opt_cat(s_reg_opts, REG_OPTS, code);
            if (desc[0]) { strlcat(s_reg_opts, " · ", REG_OPTS); opt_cat(s_reg_opts, REG_OPTS, desc); }
            strlcat(s_reg_opts, "</option>", REG_OPTS);
            cnt++;
        }
        p += 6;
        if (strlen(s_reg_opts) > REG_OPTS - 120) break;    /* 버퍼 여유 없으면 멈춤 */
    }
    if (cnt > 0) s_regs_got = true;
    free(raw);
    ESP_LOGI(TAG, "구역 목록 %d개 받음", cnt);
}

/* ---------------------------------------------------------------- 월별 집계 */
/* y년 m월(1~12)의 날 수 */
static int month_days(int y, int m)
{
    struct tm t = { 0 };
    t.tm_year = y - 1900; t.tm_mon = m; t.tm_mday = 0; t.tm_hour = 12;    /* 다음 달 0일 = 이 달 마지막 날 */
    mktime(&t);
    return t.tm_mday;
}

/* y년 m월의 검침일(날짜). "말일"이거나 그 달에 없는 날이면 그 달 마지막 날 */
static int bill_dom(int y, int m)
{
    int n = month_days(y, m);
    return s_billday > n ? n : s_billday;
}

/* 청구월 N = 오늘이 이번 달 검침일을 지났으면 다음 달, 아니면(검침일 당일 포함) 이번 달.
 * 기간 = (N-1)월 검침일 다음 날 ~ N월 검침일. 말일이면 N월 1일 ~ N월 말일. */
static uint16_t period_key(long now)
{
    struct tm tmv;
    time_t t = (time_t)now;
    localtime_r(&t, &tmv);
    int y = tmv.tm_year + 1900, mo = tmv.tm_mon + 1, d = tmv.tm_mday;
    if (d > bill_dom(y, mo)) { if (++mo > 12) { mo = 1; y++; } }
    return (uint16_t)(y * 12 + (mo - 1));
}

/* 청구월 ym 의 사용기간(from~to, "YYYY-MM-DD"). */
static void period_dates(uint16_t ym, char from[11], char to[11])
{
    int y = ym / 12, m = ym % 12 + 1;
    int py = y, pm = m - 1;
    if (pm < 1) { pm = 12; py--; }
    struct tm s = { 0 }, e = { 0 };
    s.tm_year = py - 1900; s.tm_mon = pm - 1; s.tm_mday = bill_dom(py, pm) + 1; s.tm_hour = 12;   /* 넘치면 다음 달 1일 */
    mktime(&s); strftime(from, 11, "%Y-%m-%d", &s);
    e.tm_year = y - 1900; e.tm_mon = m - 1; e.tm_mday = bill_dom(y, m); e.tm_hour = 12;
    mktime(&e); strftime(to, 11, "%Y-%m-%d", &e);
}

/* 청구월 ym 의 검침일(= N월 검침일, 마감 유예 기준) epoch. */
static time_t billday_epoch(uint16_t ym)
{
    int y = ym / 12, m = ym % 12 + 1;
    struct tm b = { 0 };
    b.tm_year = y - 1900; b.tm_mon = m - 1; b.tm_mday = bill_dom(y, m); b.tm_hour = 12;
    return mktime(&b);
}

/* 지금 쓸 기준(설정 + 무엇이 있나). 없으면 0 */
static int src_pick(void)
{
    bool meter = hub_plugin_enabled("gas");
    bool boiler = hub_rinnai_has_boiler(NULL);
    switch (s_src_cfg) {
    case SRC_METER:  return meter ? SRC_METER : 0;
    case SRC_BOILER: return boiler ? SRC_BOILER : 0;
    default:         return meter ? SRC_METER : (boiler ? SRC_BOILER : 0);
    }
}

/* 기준의 지금 누적(L). 보일러는 want(이 기간에 세던 보일러)를 먼저, 없으면 가장 최근에 누계를 올린 보일러.
 * 값을 아직 모르면(계량기 멈춤·부팅 뒤 보일러 누계가 아직 안 옴 — 약 1시간마다 온다) false */
static bool src_total(int src, const uint8_t *want, uint32_t *l, uint8_t mac3[3])
{
    memset(mac3, 0, 3);
    if (src == SRC_METER) {
        gas_view_t g; uint32_t step;
        if (!hub_sensors_gas(&g, &step)) return false;
        *l = g.count * step;
        return true;
    }
    if (src == SRC_BOILER) {
        hub_stg_t st;
        if (!(want && hub_rinnai_stg_of(want, &st)) && !hub_rinnai_stg(&st)) return false;
        *l = ((uint32_t)st.heat_gas_m3 + st.hw_gas_m3) * 1000u;     /* 난방 + 온수(정수 ㎥) */
        memcpy(mac3, st.mac3, 3);
        return true;
    }
    return false;
}

/* 직전에 본 누계(기준·보일러 MAC 과 함께) — 누계 감소 판정용, s_lock 아래에서만 */
static bool s_last_ok;
static uint8_t s_last_src, s_last_mac[3];
static uint32_t s_last_l;

static void months_update(void)
{
    long now = time(NULL);
    if (!CLOCK_OK(now)) return;
    int src = src_pick();
    if (!src) return;
    uint8_t want[3] = { 0 };
    bool has_want = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mon_n && s_mon[s_mon_n - 1].src == SRC_BOILER) {
        memcpy(want, s_mon[s_mon_n - 1].mac3, 3);
        has_want = true;
    }
    xSemaphoreGive(s_lock);
    uint32_t cur_l;
    uint8_t mac3[3];
    if (!src_total(src, has_want ? want : NULL, &cur_l, mac3)) return;   /* 다른 잠금(센서·보일러)은 이 잠금 밖에서 */
    uint16_t key = period_key(now);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool dirty = false;
    tmon_t *cur = s_mon_n ? &s_mon[s_mon_n - 1] : NULL;
    /* 누계가 줄었나는 기간 시작값이 아니라 '직전에 본 값'과 비교한다: 100→106→103 이면 이미 센 6 ㎥ 를 두고
     * 103 부터 다시 센다(시작값과만 비교하면 103-100=3 으로 줄어든다). 직전 값은 RAM 에만 — 재부팅 직후는 시작값으로 */
    bool dropped = s_last_ok && s_last_src == src && !memcmp(s_last_mac, mac3, 3) && cur_l < s_last_l;
    s_last_ok = true;
    s_last_src = (uint8_t)src;
    memcpy(s_last_mac, mac3, 3);
    s_last_l = cur_l;
    bool same = cur && cur->src == src && !memcmp(cur->mac3, mac3, 3) && cur_l >= cur->start_l && !dropped;
    if (!cur || cur->ym != key) {
        if (cur) {                        /* 이전 기간 끝 → 마감 대기(final=0). 기준이 바뀌었으면 마지막으로 센 값 그대로 */
            if (same) cur->use_l = cur->carry_l + (cur_l - cur->start_l);
            cur->won    = won_of(cur->use_l / 1000.0);
            cur->closed = 1;
            cur->final  = 0;
        }
        if (s_mon_n == MON_MAX) {
            memmove(&s_mon[0], &s_mon[1], sizeof(tmon_t) * (MON_MAX - 1));
            s_mon_n--;
        }
        cur = &s_mon[s_mon_n++];
        memset(cur, 0, sizeof(*cur));
        cur->ym = key; cur->src = (uint8_t)src; memcpy(cur->mac3, mac3, 3); cur->start_l = cur_l;
        dirty = true;
    } else if (!same) {
        /* 기준이 바뀜(설정·계량기 켜고 끔·다른 보일러) 또는 누계가 줄어듦(교체·초기화):
         * 그때까지 센 양은 두고 새 기준의 지금 값부터 이어 센다 */
        ESP_LOGI(TAG, "사용량 기준 %s → %s (이번 기간 %.1f ㎥ 이어서)", cur->src == SRC_BOILER ? "보일러" : "계량기",
                 src == SRC_BOILER ? "보일러" : "계량기", cur->use_l / 1000.0);
        cur->carry_l = cur->use_l;
        cur->src = (uint8_t)src;
        memcpy(cur->mac3, mac3, 3);
        cur->start_l = cur_l;
        dirty = true;
    }
    uint32_t use = cur->carry_l + (cur_l - cur->start_l);   /* 진행 중 라이브 */
    /* 보일러 누계는 허브가 다시 켜진 뒤 한 시간쯤 지나야 오므로 그동안 보여 줄 값을 저장해 둔다(바뀔 때만 — 1시간에 한 번꼴).
     * 계량기는 펄스 수가 허브 NVS 에 남아 켜자마자 다시 셀 수 있어 저장하지 않는다(펄스마다 쓰면 플래시가 닳는다) */
    if (src == SRC_BOILER && use != cur->use_l) dirty = true;
    cur->use_l = use;
    cur->won   = won_of(cur->use_l / 1000.0);
    xSemaphoreGive(s_lock);
    if (dirty) mon_save();
}

/* 마감 대기(closed && !final) 중 가장 최근 달을 확정 등록 시도.
 *  - /api/bill?from&to 청구액 = 청구서 재현(사용기간 가중평균).
 *  - 열량기준이 "사용기간…"(정확 열량)이면 확정. 아니면 검침일+유예일 지나면 최신 월값으로 확정. */
static void finalize_pending(void)
{
    long now = time(NULL);
    if (!CLOCK_OK(now)) return;
    int64_t nowu = esp_timer_get_time();
    if (s_fin_last_us && nowu - s_fin_last_us < (int64_t)FIN_THROTTLE_S * 1000000) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int j = -1;
    for (int i = s_mon_n - 1; i >= 0; i--) if (s_mon[i].closed && !s_mon[i].final) { j = i; break; }
    tmon_t m = (j >= 0) ? s_mon[j] : (tmon_t){ 0 };
    xSemaphoreGive(s_lock);
    if (j < 0) return;
    s_fin_last_us = nowu;

    char from[11], to[11];
    period_dates(m.ym, from, to);
    double m3 = m.use_l / 1000.0;
    char url[160];
    snprintf(url, sizeof(url), "https://gas.yangsu.kim/api/bill?region=%s&m3=%.2f&from=%s&to=%s",
             s_region, m3, from, to);
    if (http_get(url, s_buf, BUF_MAX) < 0) return;
    double bill = 0;
    if (!num_after(s_buf, "\"청구액\"", &bill)) return;
    char basis[72] = "";
    str_after(s_buf, "\"열량기준\"", basis, sizeof(basis));
    bool accurate = strstr(basis, "사용기간") != NULL;     /* 사용기간 가중평균 = 정확 열량 확정 */
    bool grace_over = now >= billday_epoch(m.ym) + (time_t)FIN_GRACE_D * 86400;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (j < s_mon_n && s_mon[j].ym == m.ym) {
        s_mon[j].won = (uint32_t)llround(bill);
        s_mon[j].final = (accurate || grace_over) ? 1 : 0;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "마감 %04d-%02d: 청구 %.0f (%s)%s", m.ym / 12, m.ym % 12 + 1, bill, basis,
             (accurate || grace_over) ? " 확정" : " 잠정");
    mon_save();
}

/* ---------------------------------------------------------------- 태스크 */
static void tariff_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(FIRST_DELAY_S * 1000));
    int64_t last_fetch_us = 0;
    for (;;) {
        /* 요금 서버(구역 목록·단가 받기, 기간 끝 청구액 확인)는 셀 것이 있을 때만 — 가스 계량기 플러그인을 켰거나
         * 보일러를 아는 허브. 위성 허브(방만)·센서만 쓰는 허브는 밖으로 아무것도 보내지 않는다 */
        if (!hub_plugin_enabled("gas") && !hub_rinnai_has_boiler(NULL)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60 * 1000));
            continue;
        }
        if (!s_regs_got) fetch_regions();
        months_update();
        bool need_retry = false;
        if (s_mode == MODE_AUTO) {
            int64_t nowu = esp_timer_get_time();
            bool want = s_fet_want;
            if (want || !s_fet_valid || nowu - last_fetch_us > (int64_t)FETCH_OK_S * 1000000) {
                s_fet_want = false;
                s_fet_busy = true;
                if (fetch_auto()) {
                    last_fetch_us = nowu;
                    s_fet_fails = 0;
                } else {
                    /* 새로고침이었거나 받아 둔 값이 없으면 곧 다시(서버가 502 를 한 번 내고 다음엔 답하는 일이 있다) */
                    need_retry = want || !s_fet_valid;
                    if (need_retry) {
                        s_fet_want = true;
                        if (s_fet_fails < 250) s_fet_fails++;
                    }
                }
                s_fet_busy = false;
            }
        } else {
            s_fet_want = false;
            s_fet_busy = false;
        }
        relay_current();
        if (s_bill_want) {
            s_bill_want = false;
            hub_tariff_send_bill();
        }
        finalize_pending();
        uint32_t wait = s_mon_n == 0 ? 30 : WAKE_S;
        if (need_retry) {              /* 15초, 30초, 1분 … 최대 10분 */
            wait = FETCH_RETRY_S << (s_fet_fails > 6 ? 6 : s_fet_fails - 1);
            if (wait > WAKE_S) wait = WAKE_S;
        }
        s_fet_retry_s = s_fet_err[0] ? wait : 0;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait * 1000));
    }
}

void hub_tariff_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    load_cfg();
    mon_load();
    if (!s_task) xTaskCreate(tariff_task, "hub_tariff", 8192, NULL, 3, &s_task);
}

/* ---------------------------------------------------------------- 포털 저장 */
void hub_tariff_refresh(void)
{
    if (s_mode != MODE_AUTO) return;
    /* 받아 둔 값은 버리지 않는다 — 새 값이 올 때까지 그 값으로 계산한다(실패해도 수동값으로 떨어지지 않게) */
    s_fet_want  = true;
    s_fet_busy  = true;
    s_fet_fails = 0;
    s_relay_sig = 0xFFFFFFFFu;
    if (s_task) xTaskNotifyGive(s_task);
}

int hub_tariff_apply(int mode, int src, const char *region, int billday,
                     double vcf, double heat, double rate, double base,
                     char *err, int errlen)
{
#define FAIL(m) do { if (err) strlcpy(err, (m), errlen); return 0; } while (0)
    if (billday < 1 || billday > BILLDAY_LAST) FAIL("검침일은 1~31일 또는 말일");
    char rg[12] = "";
    if (region) {
        for (const char *p = region; *p && (size_t)(p - region) < sizeof(rg) - 1; p++) {
            char ch = *p;
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')))
                FAIL("구역 코드는 영문·숫자만 (예: SD07)");
            rg[p - region] = (ch >= 'a' && ch <= 'z') ? ch - 32 : ch;
        }
    }
    if (!rg[0]) FAIL("구역 코드를 넣어 주세요 (예: SD07)");
    if (base < 0 || base > 100000) FAIL("기본료 범위 벗어남");
    if (src < SRC_AUTO || src > SRC_BOILER) FAIL("사용량 기준이 잘못됨");
    if (mode == MODE_MANUAL) {
        if (vcf < 0.9 || vcf > 1.1) FAIL("보정계수는 0.9~1.1");
        if (heat < 20 || heat > 60) FAIL("열량은 20~60 MJ/N㎥");
        if (rate <= 0 || rate > 100) FAIL("단가는 0~100 원/MJ");
    }
    s_mode    = mode == MODE_MANUAL ? MODE_MANUAL : MODE_AUTO;
    s_src_cfg = (uint8_t)src;
    s_billday = (uint8_t)billday;
    strlcpy(s_region, rg, sizeof(s_region));
    s_man_base = (uint32_t)lround(base);         /* 기본료 = 내 정보(항상 저장) */
    if (mode == MODE_MANUAL) {
        s_man_vcf_e6  = (uint32_t)lround(vcf * 1e6);
        s_man_heat_e6 = (uint32_t)lround(heat * 1e6);
        s_man_rate_e6 = (uint32_t)lround(rate * 1e6);
    }
    save_cfg();
    s_fet_valid = false;
    s_fet_err[0] = 0;
    s_fet_fails = 0;
    s_fet_busy  = (s_mode == MODE_AUTO);
    s_relay_sig = 0xFFFFFFFFu;
    if (s_task) xTaskNotifyGive(s_task);
    s_bill_want = true;                          /* 기준을 바꿨으면 보일러 카드도 바로(보내기는 태스크가) */
    return 1;
#undef FAIL
}

/* ---------------------------------------------------------------- API/metrics */
void hub_tariff_cur(int *mode, const char **region, int *billday,
                    double *vcf, double *heat, double *rate, double *base, long *last_fetch)
{
    uint32_t v, h, r, b;
    eff_e6(&v, &h, &r, &b);
    if (mode) *mode = s_mode;
    if (region) *region = s_region;
    if (billday) *billday = s_billday;
    if (vcf) *vcf = v / 1e6;
    if (heat) *heat = h / 1e6;
    if (rate) *rate = r / 1e6;
    if (base) *base = b;
    if (last_fetch) *last_fetch = s_fet_valid ? s_fet_epoch : 0;
}

void hub_tariff_tick(void) { months_update(); }

/* 이번 청구 기간의 사용량(L)과 예상 청구액(원) — 관리 화면 그래프의 마지막 달과 같은 값. 스마트홈에도 이 값을 보낸다
 * (드라이버가 따로 세면 시작점이 달라 웹과 앱의 숫자가 어긋난다). 아직 모르면(시계 전·가스 꺼짐) false. */
bool hub_tariff_period(uint32_t *use_l, uint32_t *won, int *src)
{
    if (!s_lock) return false;
    months_update();
    long now = time(NULL);
    if (!CLOCK_OK(now)) return false;               /* 시계 전엔 '이번 기간'이 언제인지 모른다 */
    uint16_t key = period_key(now);
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* 마지막 기록이 지금 기간 것일 때만. 기간이 바뀌었는데 셀 기준이 없어(보일러·계량기 모두 없음) 새 줄이 안 생겼으면
     * 마지막 줄은 지난 기간 값이다 — 그걸 '이번달'로 보내지 않고 모름으로 */
    if (s_mon_n && !s_mon[s_mon_n - 1].closed && s_mon[s_mon_n - 1].ym == key) {
        *use_l = s_mon[s_mon_n - 1].use_l;
        *won = s_mon[s_mon_n - 1].won;
        if (src) *src = s_mon[s_mon_n - 1].src;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

int hub_tariff_source(int *cfg)
{
    if (cfg) *cfg = s_src_cfg;
    return src_pick();
}

/* 보일러 카드용(CMD 0x0f): [기준][이번 기간 L][예상 청구액 원], 모르면 [0]. 같은 값이어도 보낸다(연결 목록과 같이 5분마다·
 * refresh — 드라이버가 다시 켜져도 곧 채워지게). 위성 허브(보일러 없음)는 기준이 없어 [0] 만 — 중심 허브는 위성의 0x0f 를 버린다.
 * '지금 셀 수 있나'(src_pick)로 거르지 않는다: 부팅 직후엔 보일러가 아직 안 붙어 있어 구역이 잠깐 사라졌다 돌아온다.
 * 진행 중인 기간 기록이 있으면 그 값(마지막으로 센 값)을 보낸다 */
void hub_tariff_send_bill(void)
{
    if (!s_lock) return;                                /* 시작 전 — 다음 바퀴(5분)에 */
    uint8_t b[9] = { 0 };
    uint32_t use_l = 0, won = 0;
    int src = 0;
    if (!hub_tariff_period(&use_l, &won, &src)) src = 0;
    if (src == SRC_METER || src == SRC_BOILER) {
        b[0] = (uint8_t)src;
        memcpy(b + 1, &use_l, 4);                       /* 리틀엔디언 */
        memcpy(b + 5, &won, 4);
    }
    hub_uplink_send(HUB_RK_CMD_BILL, b, b[0] ? sizeof(b) : 1);
}

int hub_tariff_months(void) { return s_mon_n; }

int hub_tariff_month_at(int i, int *year, int *month, double *m3, uint32_t *won, int *closed, int *src)
{
    if (i < 0 || i >= s_mon_n) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tmon_t m = s_mon[i];
    xSemaphoreGive(s_lock);
    if (year) *year = m.ym / 12;
    if (month) *month = m.ym % 12 + 1;
    if (m3) *m3 = m.use_l / 1000.0;
    if (won) *won = m.won;
    if (closed) *closed = m.final ? 2 : (m.closed ? 1 : 0);    /* 0 진행 1 잠정 2 확정 */
    if (src) *src = m.src;
    return 1;
}

/* ---------------------------------------------------------------- 관리 화면 */
static void catf(char *out, size_t cap, const char *fmt, ...)
{
    size_t o = strlen(out);
    if (o >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + o, cap - o, fmt, ap);
    va_end(ap);
}

/* 온압보정계수 적용 고시일(연 1회 6/1) */
static void vcf_basis(long now, char *out, size_t cap)
{
    struct tm t;
    time_t tt = (time_t)now;
    localtime_r(&tt, &t);
    int y = t.tm_year + 1900;
    if (t.tm_mon + 1 < 6) y--;                 /* 6월 전이면 작년 6/1 고시 */
    snprintf(out, cap, "%d년 6월 1일 고시", y);
}

/* 열량 기준 글자를 사람이 읽기 좋게 바꾼다(서버가 주는 꼴 → 화면 글).
 *   "month 20261119 (2026-10-19~2026-11-19 (api-range))" → "2026년 10월 19일~11월 19일 평균"
 *   "사용기간 2026-10-08~2026-11-07 (가중평균)"            → "2026년 10월 8일~11월 7일 사용기간 가중평균"
 * 날짜를 못 찾으면 받은 글자 그대로(이스케이프해서). */
static void heat_basis_text(char *out, size_t cap)
{
    out[0] = 0;
    for (const char *p = s_fet_heatbasis; *p; p++) {
        int y1, m1, d1, y2, m2, d2;
        if (*p < '0' || *p > '9' || sscanf(p, "%4d-%2d-%2d~%4d-%2d-%2d", &y1, &m1, &d1, &y2, &m2, &d2) != 6 ||
            m1 < 1 || m1 > 12 || m2 < 1 || m2 > 12 || d1 < 1 || d1 > 31 || d2 < 1 || d2 > 31) {
            continue;
        }
        const char *kind = strstr(s_fet_heatbasis, "가중평균") ? "사용기간 가중평균" : "평균";
        if (y1 == y2) {
            snprintf(out, cap, "%d년 %d월 %d일~%d월 %d일 %s", y1, m1, d1, m2, d2, kind);
        } else {
            snprintf(out, cap, "%d년 %d월 %d일~%d년 %d월 %d일 %s", y1, m1, d1, y2, m2, d2, kind);
        }
        return;
    }
    opt_cat(out, cap, s_fet_heatbasis);
}

/* '현재값' 줄들 — 관리 화면 카드와 /admin/gas-now(페이지를 다시 그리지 않는 새로고침)가 같이 쓴다.
 * 노출 설정 카드와 같은 모양(.row: 왼쪽 이름, 오른쪽 값·기준). out 뒤에 덧붙인다. */
void hub_tariff_now_html(char *out, size_t cap)
{
    /* <i id=gw> = 아직 끝나지 않았다는 표시: 화면이 이게 사라질 때까지 이 조각을 다시 받아 온다 */
    bool busy = s_fet_busy;
    if (busy) {
        catf(out, cap, "<div class=row><span class=muted>%s</span></div><i id=gw hidden></i>",
             s_fet_valid ? "다시 받는 중…" : "받는 중…");
    } else if (s_fet_err[0]) {
        char when[96] = "";
        uint32_t r = s_fet_retry_s;
        if (r) {
            snprintf(when, sizeof(when), r < 60 ? " · %lu초 뒤 다시 받습니다<i id=gw hidden></i>" : " · %lu분 뒤 다시 받습니다",
                     (unsigned long)(r < 60 ? r : r / 60));
        }
        catf(out, cap, "<div class=row><span class=muted>⚠ 받지 못함 (%s)%s</span></div>", s_fet_err, when);
    }
    if (!s_fet_valid) {
        if (!busy && !s_fet_err[0]) {
            catf(out, cap, "<div class=row><span class=muted>받는 중… 계속 이 상태면 허브의 인터넷 연결을 확인하세요.</span></div>"
                           "<i id=gw hidden></i>");
        }
        return;
    }
    char b[128];
    vcf_basis(time(NULL), b, sizeof(b));
    catf(out, cap, "<div class=row><span>온압보정계수</span><span class=gv><b>%.4f</b><small>%s</small></span></div>",
         s_fet_vcf_e6 / 1e6, b);
    heat_basis_text(b, sizeof(b));
    catf(out, cap, "<div class=row><span>열량</span><span class=gv><b>%.3f</b> MJ/N㎥<small>%s</small></span></div>",
         s_fet_heat_e6 / 1e6, b);
    catf(out, cap, "<div class=row><span>단가</span><span class=gv><b>%.4f</b> 원/MJ</span></div>", s_fet_rate_e6 / 1e6);
    if (s_fet_regname[0]) {
        b[0] = 0;
        opt_cat(b, sizeof(b), s_fet_regname);
        catf(out, cap, "<div class=row><span>구역</span><span class=gv>%s</span></div>", b);
    }
    if (s_fet_epoch) {
        time_t tt = (time_t)s_fet_epoch;
        struct tm tm;
        localtime_r(&tt, &tm);
        catf(out, cap, "<div class=row><span>받은 시각</span><span class=gv>%d월 %d일 %02d:%02d</span></div>",
             tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    }
}

void hub_tariff_render(char *out, size_t cap)
{
    months_update();
    bool man = (s_mode == MODE_MANUAL);

    catf(out, cap, "<h2>가스 요금</h2><div class=card>");

    /* 월별 그래프 */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_mon_n;
    tmon_t mv[MON_MAX];
    memcpy(mv, s_mon, sizeof(mv));
    xSemaphoreGive(s_lock);
    int show = n > 12 ? 12 : n, offb = n - show;
    if (show > 0) {
        uint32_t mx = 1;
        for (int i = offb; i < n; i++) if (mv[i].won > mx) mx = mv[i].won;
        /* 막대 폭은 12칸 고정 — 기록된 달은 왼쪽부터 세우고, 나머지 오른쪽 칸은 빈 공간으로 둔다
         * (한 달만 있을 때 막대가 폭 전체로 넓어지지 않게). */
        const int SLOTS = 12;
        const int W = 320, H = 150, PADB = 24, PADT = 14, bw = (W - 10) / SLOTS;
        catf(out, cap, "<svg viewBox='0 0 %d %d' style='width:100%%;height:auto' role='img' aria-label='월별 가스요금'>", W, H);
        catf(out, cap, "<line x1=5 y1=%d x2=%d y2=%d stroke='#333'/>", H - PADB, 5 + SLOTS * bw, H - PADB);
        for (int i = offb; i < n; i++) {
            int idx = i - offb, x = 5 + idx * bw;
            int bh = (int)((double)mv[i].won * (H - PADB - PADT) / mx);
            if (bh < 0) bh = 0;
            int y = H - PADB - bh;
            const char *fill = mv[i].final ? "#c33" : (mv[i].closed ? "#e66" : "#e88");
            catf(out, cap, "<rect x=%d y=%d width=%d height=%d rx=2 fill='%s'/>", x + 2, y, bw - 4, bh, fill);
            catf(out, cap, "<text x=%d y=%d font-size=9 fill='#bbb' text-anchor=middle>%d월</text>",
                 x + bw / 2, H - 8, mv[i].ym % 12 + 1);
            if (mv[i].won > 0)
                catf(out, cap, "<text x=%d y=%d font-size=9 fill='#eee' text-anchor=middle>%u</text>",
                     x + bw / 2, y - 3, (unsigned)mv[i].won);
        }
        catf(out, cap, "</svg><p class=muted style='margin:4px 0 0'>월별 청구액(원, 부가세 포함). "
                       "진한색=확정, 중간=잠정(열량 확정 전), 옅은색=진행 중.</p>");
    } else {
        catf(out, cap, "<p class=muted>아직 월별 기록이 없습니다 (시간 동기화·가스 사용 후 쌓입니다).</p>");
    }

    /* ---- 설정 폼 ---- */
    catf(out, cap, "<form method=post action=/admin/gas>");
    /* 사용량 기준: 한 줄(라벨 + 자동/계량기/보일러) + 지금 무엇으로 세나 */
    {
        bool meter = hub_plugin_enabled("gas");
        bool boiler = hub_rinnai_has_boiler(NULL);
        int used = src_pick();
        catf(out, cap, "<div class=gline><b>사용량 기준</b>"
                       "<label class=gopt><input type=radio name=src value=auto %s> 자동</label>"
                       "<label class=gopt><input type=radio name=src value=meter %s> 가스 계량기%s</label>"
                       "<label class=gopt><input type=radio name=src value=boiler %s> 보일러 누계%s</label></div>",
             s_src_cfg == SRC_AUTO ? "checked" : "", s_src_cfg == SRC_METER ? "checked" : "", meter ? "" : " (없음)",
             s_src_cfg == SRC_BOILER ? "checked" : "", boiler ? "" : " (없음)");
        catf(out, cap, "<p class='muted sub'>");
        if (s_src_cfg == SRC_AUTO) catf(out, cap, "자동 = 계량기가 있으면 계량기, 없으면 보일러. ");
        if (used == SRC_METER) {
            catf(out, cap, "지금: <b>가스 계량기</b> 펄스로 셉니다.");
        } else if (used == SRC_BOILER) {
            hub_stg_t st;
            catf(out, cap, "지금: <b>보일러 누계</b>(보일러가 스스로 센 난방+온수 가스, 약 1시간마다 1 ㎥ 단위)로 셉니다. "
                           "실제 계량기와 다를 수 있는 예상값입니다.");
            if (!hub_rinnai_stg(&st)) catf(out, cap, " <b>아직 누계를 받지 못했습니다</b>(허브가 켜진 뒤 1시간 안에 옵니다).");
        } else {
            catf(out, cap, "⚠ 지금은 셀 수 없습니다: %s",
                 s_src_cfg == SRC_METER ? "가스 계량기 플러그인이 꺼져 있습니다." :
                 s_src_cfg == SRC_BOILER ? "연결된 보일러가 없습니다." : "가스 계량기도 보일러도 없습니다.");
        }
        catf(out, cap, "</p>");
    }
    /* 조회 방식: 한 줄(라벨 + 자동/수동) */
    catf(out, cap, "<div class=gline><b>가스 요금 조회</b>"
                   "<label class=gopt><input type=radio name=mode value=auto %s onchange=_gm()> 자동 (gas.yangsu.kim)</label>"
                   "<label class=gopt><input type=radio name=mode value=manual %s onchange=_gm()> 수동</label></div>",
         man ? "" : "checked", man ? "checked" : "");

    /* 아래 상자들은 '노출 설정' 카드와 같은 모양(.grpbox 제목 + .row 왼쪽 이름/오른쪽 값) */
    /* 자동: 구역 + 현재값(새로고침은 페이지 유지, 값만 갱신) */
    catf(out, cap, "<div id=gauto><div class=grpbox><h3 class=grp>📍 구역</h3><p class='muted sub'>%s</p>",
         s_regs_got ? "목록에서 지역·공급사를 검색해 고르거나 코드를 직접 입력 (예: SD07)"
                    : "구역 목록 받는 중… 코드를 직접 입력 (예: SD07)");
    if (s_regs_got) {
        catf(out, cap, "<input name=region list=gregions value=\"%s\" autocapitalize=characters "
                       "placeholder='검색: 지역·공급사'><datalist id=gregions>%s</datalist>", s_region, s_reg_opts);
    } else {
        catf(out, cap, "<input name=region value=\"%s\" maxlength=11 autocapitalize=characters>", s_region);
    }
    catf(out, cap, "</div><div class=grpbox><div class=ptitle><h3 class=grp>📊 현재값</h3>"
                   "<button type=button class=lnk onclick=_grefresh() title='지금 다시 받기'>↻ 새로고침</button></div>"
                   "<p class='muted sub'>gas.yangsu.kim 에서 자동으로 받은 값</p><div id=gnow>");
    hub_tariff_now_html(out, cap);
    catf(out, cap, "</div></div></div>");

    /* 수동: 직접 넣는 값 */
    catf(out, cap, "<div id=gman class=grpbox><h3 class=grp>✏️ 수동값</h3>"
                   "<p class='muted sub'>직접 넣은 값으로 계산 (인터넷 불필요)</p>");
    catf(out, cap, "<div class=row><span>온압보정계수</span><input class=gin name=vcf inputmode=decimal value=\"%.4f\"></div>",
         s_man_vcf_e6 / 1e6);
    catf(out, cap, "<div class=row><span>열량 (MJ/N㎥)</span><input class=gin name=heat inputmode=decimal value=\"%.3f\"></div>",
         s_man_heat_e6 / 1e6);
    catf(out, cap, "<div class=row><span>단가 (원/MJ)</span><input class=gin name=rate inputmode=decimal value=\"%.4f\"></div>",
         s_man_rate_e6 / 1e6);
    catf(out, cap, "</div>");

    /* 내 정보(항상 편집) */
    catf(out, cap, "<div class=grpbox><h3 class=grp>👤 내 정보</h3><p class='muted sub'>자동·수동 모두에 쓰는 값</p>");
    catf(out, cap, "<div class=row><span>기본요금 (원/월)</span><input class=gin name=base inputmode=numeric value=\"%u\"></div>",
         (unsigned)s_man_base);
    catf(out, cap, "<div class=row><span>검침일</span><select class=gin name=billday>");
    for (int d = 1; d <= 31; d++) {
        catf(out, cap, "<option value=%d%s>%d일</option>", d, d == s_billday ? " selected" : "", d);
    }
    catf(out, cap, "<option value=%d%s>말일</option></select></div>", BILLDAY_LAST, s_billday == BILLDAY_LAST ? " selected" : "");
    catf(out, cap, "</div>");
    catf(out, cap, "<button class=sv>저장</button></form>");
    if (s_billday == BILLDAY_LAST) {
        catf(out, cap, "<p class=muted style='margin:8px 0 0'>요금 기간: 매월 <b>1일</b>부터 <b>말일</b>까지. "
                       "즉 'N월' 막대 = N월 1일 ~ N월 말일 사용분입니다.</p>");
    } else if (s_billday <= 27) {
        catf(out, cap, "<p class=muted style='margin:8px 0 0'>요금 기간: 전달 검침일 <b>다음 날</b>부터 이번 달 <b>검침일</b>까지. "
                       "즉 'N월' 막대 = 전달 %u일 ~ N월 %u일 사용분입니다.</p>", s_billday + 1, s_billday);
    } else {                                         /* 28~31일: 그 날이 없는 달이 있다 */
        catf(out, cap, "<p class=muted style='margin:8px 0 0'>요금 기간: 전달 검침일 <b>다음 날</b>부터 이번 달 <b>검침일(%u일)</b>까지. "
                       "%u일이 없는 달에는 그 달의 마지막 날을 검침일로 봅니다.</p>", s_billday, s_billday);
    }

    /* 스타일(이 카드에서만 쓰는 것) */
    catf(out, cap, "<style>.gline{display:flex;align-items:center;gap:10px 14px;flex-wrap:wrap;margin:6px 0}"
                   ".gline b{white-space:nowrap}"
                   ".gline .gopt{display:inline-flex;align-items:center;gap:4px;margin:0;white-space:nowrap;flex:none;color:#ddd}"
                   ".gline .gopt input{width:auto;padding:0;margin:0;flex:none}"
                   ".grpbox .row{align-items:center}.gv{text-align:right}"
                   ".gv small{display:block;color:#888;font-size:12px}"
                   "input.gin,select.gin{width:8.5em;text-align:right;padding:8px 10px}"
                   "select.gin{font-size:16px;border-radius:8px;border:1px solid #444;background:#1c1c1c;color:#eee}"
                   "input[inputmode=decimal],input[inputmode=numeric]{appearance:textfield;-moz-appearance:textfield}"
                   "input::-webkit-outer-spin-button,input::-webkit-inner-spin-button{-webkit-appearance:none;margin:0}</style>");

    /* 토글(자동↔수동 상호배타) + 비동기 새로고침(페이지 유지, 값만 갱신) */
    catf(out, cap, "<script>"
                   "function _gm(){var m=document.querySelector('input[name=mode][value=manual]').checked;"
                   "document.getElementById('gman').style.display=m?'':'none';"
                   "document.getElementById('gauto').style.display=m?'none':'';"
                   "document.getElementById('gman').querySelectorAll('input').forEach(function(i){i.disabled=!m});}"
                   "function _gput(u){fetch(u).then(function(r){return r.text()}).then(function(t){"
                   "document.getElementById('gnow').innerHTML=t}).catch(function(){});}"
                   /* 값이 올 때까지(조각에 gw 표시가 있는 동안) 3초마다 다시 받아 온다. 최대 3분 */
                   "var _gt;function _gpoll(){clearInterval(_gt);var k=0;_gt=setInterval(function(){"
                   "if(++k>60||!document.getElementById('gw')){clearInterval(_gt);return}_gput('/admin/gas-now')},3000);}"
                   "function _grefresh(){document.getElementById('gnow').innerHTML="
                   "'<span style=opacity:.6>받는 중…</span><i id=gw hidden></i>';_gput('/admin/gas-now?do=refresh');_gpoll();}"
                   "_gm();if(document.getElementById('gw'))_gpoll();</script>");
    catf(out, cap, "</div>");
}
