/*
 * hub_bbox.c -- 부팅 기록(블랙박스): 재시작에도 남는 로그 + 무선 카운터 + 프레임 카운터 + 부팅 이력. 설명은 hub_bbox.h.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_zigbee.h"
#include "ezbee/nwk.h"
#include "ezbee/bdb.h"
#include "ezbee/platform/log.h"
#include "sdkconfig.h"
#if CONFIG_SOC_IEEE802154_SUPPORTED
#include "soc/ieee802154_struct.h"
#endif

#include "hub_bbox.h"
#include "esp_heap_caps.h"
#include "hub_proxy.h"
#include "hub_zigbee.h"
#include "hub_zb_rinnai.h"
#include "hub_wifi.h"
#include "hub_ota.h"

static const char *TAG = "HUB_BBOX";

/* Zigbee 와 관련된 줄(스택·가입 처리·이 모듈의 메모)은 따로 담는다. 한 버퍼에 섞었더니 Wi-Fi·프록시 로그에 밀려
 * 정작 재가입 줄이 남지 않았다(2026-11-21 첫 시험). */
#define ZHEAD_CAP 2560          /* Zigbee 줄: 부팅 앞부분(덮어쓰지 않음) */
#define ZTAIL_CAP 5120          /* Zigbee 줄: 마지막 부분(고리) */
#define GTAIL_CAP 2560          /* 그 밖의 줄: 마지막 부분(고리) */
#define BB_LINE 200
#define MAGIC    0x62627832u    /* "bbx2" */
#define HIST_N   48
#define FC_TICK_MS 5000

typedef struct {
    uint32_t boot_no;
    uint16_t zhead_len;
    uint16_t rsv;
    uint32_t ztail_total;       /* 고리에 지금까지 쓴 바이트 */
    uint32_t gtail_total;
    char zhead[ZHEAD_CAP];
    char ztail[ZTAIL_CAP];
    char gtail[GTAIL_CAP];
} slot_t;

typedef struct {
    uint32_t magic;
    uint8_t  cur;               /* 이번 부팅이 쓰는 칸(0/1). 반대 칸이 직전 부팅 */
    uint8_t  rsv[3];
    uint32_t fc_last;           /* 마지막으로 본 프레임 카운터(이번 부팅) */
    uint32_t fc_up_s;           /* 그때가 부팅 후 몇 초였나 */
    slot_t   s[2];
    uint32_t magic2;
} nv_t;

static __NOINIT_ATTR nv_t s_nv;

/* 부팅 이력 한 건(NVS "bbox"/"hist" 에 HIST_N 건) */
typedef struct {
    uint32_t boot_no;
    uint32_t epoch;             /* 부팅 시각(벽시계, 0 = 모름) */
    uint8_t  reset_reason;
    uint8_t  slot;              /* 0 = ota_0, 1 = ota_1 */
    uint8_t  result;            /* 0 = 아직, 1 = 붙음, 2 = 붙기 전에 나감(분실), 3 = 붙었다가 나감 */
    uint8_t  fails;
    uint8_t  first_fail;
    uint8_t  leave_type;
    uint8_t  restore_try;       /* 이 부팅이 되살리기 몇 번째 재시도였나 */
    uint8_t  fc_src;            /* 직전 프레임 카운터 출처: 0 = 없음, 1 = RAM, 2 = 플래시 */
    uint16_t join_s;            /* 붙은 때(부팅 후 초, 0xffff = 없음) */
    uint16_t leave_s;
    uint32_t fc_boot;           /* 스택이 들고 올라온 프레임 카운터 */
    uint32_t fc_prev;           /* 직전 부팅에서 마지막으로 본 값 */
    uint16_t ack_to, cca_busy, cca_fail, crc;   /* 결과가 나올 때까지의 무선 카운터(넘치면 0xffff) */
} rec_t;

typedef struct {
    uint32_t fc, up_s, boot_no;
} last_t;

static SemaphoreHandle_t s_mx;          /* 로그 한 줄 만들기 */
static SemaphoreHandle_t s_hmx;         /* 부팅 이력 */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;
static char s_line[BB_LINE];
static bool s_prev_ok;                  /* 직전 부팅 기록이 남아 있나 */
static uint32_t s_prev_fc, s_prev_fc_up;
static rec_t s_hist[HIST_N];
static int s_hn;                        /* 이력 건수. 마지막이 이번 부팅 */
static bool s_started;
static volatile uint8_t s_zb_lvl = EZB_LOG_LEVEL_DEBUG;

static uint32_t up_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

/* ---------------------------------------------------------------- 로그 담기 */

static void ring_put(char *buf, int cap, uint32_t *total, const char *p, int n)
{
    while (n > 0) {
        int at = (int)(*total % (uint32_t)cap), k = cap - at;
        if (k > n) {
            k = n;
        }
        memcpy(buf + at, p, (size_t)k);
        *total += (uint32_t)k;
        p += k;
        n -= k;
    }
}

static void put(bool zb, const char *p, int n)
{
    slot_t *c = &s_nv.s[s_nv.cur];
    portENTER_CRITICAL(&s_mux);
    if (!zb) {
        ring_put(c->gtail, GTAIL_CAP, &c->gtail_total, p, n);
    } else {
        if (c->zhead_len < ZHEAD_CAP) {
            int k = ZHEAD_CAP - c->zhead_len;
            if (k > n) {
                k = n;
            }
            memcpy(c->zhead + c->zhead_len, p, (size_t)k);
            c->zhead_len = (uint16_t)(c->zhead_len + k);
            p += k;
            n -= k;
        }
        ring_put(c->ztail, ZTAIL_CAP, &c->ztail_total, p, n);
    }
    portEXIT_CRITICAL(&s_mux);
}

/* Zigbee 쪽 버퍼에 담을 줄인가. ESP 로그 한 줄은 "I (1234) TAG: 내용" 꼴이다. */
static bool zb_line(const char *s)
{
    static const char *const TAGS[] = { "ZB:", "TEMPERATURE_SENSOR:", "HUB_BBOX:", "HUB_OTA:", "HUB_CRYPT:" };
    const char *t = strstr(s, ") ");
    if (!t || t - s > 14) {
        return false;
    }
    t += 2;
    for (size_t i = 0; i < sizeof(TAGS) / sizeof(TAGS[0]); i++) {
        if (!strncmp(t, TAGS[i], strlen(TAGS[i]))) {
            return strstr(t, "Default Response") == NULL;      /* 보고마다 찍히는 응답 줄은 뺀다(버퍼를 다 채운다) */
        }
    }
    if (!strncmp(t, "HUB_WIFI:", 9)) {              /* Wi-Fi 를 멈추고 켠 때는 같이 봐야 한다. 10초마다 찍는 메모리 줄은 뺀다 */
        return strstr(t, "메모리") == NULL;
    }
    return false;
}

/* 한 줄을 만들어 담는다. 로그를 부르는 쪽 스택을 더 쓰지 않도록 공용 버퍼를 잠가서 쓴다. */
static void capture(const char *prefix, const char *fmt, va_list ap)
{
    if (!s_mx || xPortInIsrContext() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return;
    }
    if (xSemaphoreTake(s_mx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    int o = 0;
    if (prefix) {
        o = snprintf(s_line, sizeof(s_line), "%s (%lu) ", prefix, (unsigned long)(esp_timer_get_time() / 1000));
    }
    int n = vsnprintf(s_line + o, sizeof(s_line) - (size_t)o, fmt, ap);
    if (n > 0) {
        n += o;
        if (n >= (int)sizeof(s_line)) {
            n = sizeof(s_line) - 1;
        }
        if (prefix && s_line[n - 1] != '\n') {
            if (n == (int)sizeof(s_line) - 1) {
                n--;
            }
            s_line[n++] = '\n';
        }
        s_line[n] = 0;
        put(prefix != NULL || zb_line(s_line), s_line, n);
    }
    xSemaphoreGive(s_mx);
}

static int bbox_vprintf(const char *fmt, va_list ap)
{
    va_list c;
    va_copy(c, ap);
    capture(NULL, fmt, c);
    va_end(c);
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
}

void hub_bbox_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    capture("N", fmt, ap);
    va_end(ap);
}

/* Zigbee 스택의 로그 출구. 라이브러리의 기본 구현(약한 심볼)은 INFO 까지만 내보내고 나머지는 버린다 — 여기서 받아
 * 디버그 수준까지 기록에 담는다. INFO 이상은 예전처럼 콘솔에도 낸다(콘솔 출력은 위 bbox_vprintf 가 기록에 담는다). */
void ezb_plat_log(ezb_log_level_t level, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    if (level >= EZB_LOG_LEVEL_ERROR && level <= EZB_LOG_LEVEL_INFO) {
        static const char L[] = { 'E', 'W', 'I' };
        static const esp_log_level_t EL[] = { ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO };
        char msg[160];
        vsnprintf(msg, sizeof(msg), format, ap);
        size_t n = strlen(msg);
        while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) {
            msg[--n] = 0;
        }
        esp_log_write(EL[level - 1], "ZB", "%c (%lu) ZB: %s\n", L[level - 1], (unsigned long)esp_log_timestamp(), msg);
    } else if (level > EZB_LOG_LEVEL_INFO && level <= s_zb_lvl) {
        capture(level == EZB_LOG_LEVEL_DEBUG ? "Zd" : "Zv", format, ap);
    }
    va_end(ap);
}

/* ---------------------------------------------------------------- 무선 카운터 */

#if CONFIG_SOC_IEEE802154_SUPPORTED
typedef struct {
    uint32_t ack_to, cca_busy, cca_fail, crc, sfd_to, filt, no_rss, rx_restart, sec_err;
    uint32_t coex_rx, coex_txack, coex_rxack, coex_tx, coex_ed;
} radio_t;
static radio_t s_r_last, s_r_boot;
static bool s_r_have;
static volatile bool s_stack_up;        /* 스택이 라디오를 켰다(첫 신호를 받음) — 이 뒤에만 레지스터를 읽는다 */

static bool radio_read(radio_t *r)
{
    /* 스택이 라디오를 켠 뒤에만 레지스터를 읽는다 */
    if (!s_stack_up) {
        return false;
    }
    r->ack_to = IEEE802154.debug_rx_ack_timeout_cnt;
    r->cca_busy = IEEE802154.debug_cca_busy_cnt;
    r->cca_fail = IEEE802154.debug_cca_fail_cnt;
    r->crc = IEEE802154.debug_crc_error_cnt;
    r->sfd_to = IEEE802154.debug_sfd_timeout_cnt;
    r->filt = IEEE802154.debug_rx_filter_fail_cnt;
    r->no_rss = IEEE802154.debug_no_rss_detect_cnt;
    r->rx_restart = IEEE802154.debug_rx_restart_cnt;
    r->sec_err = IEEE802154.debug_tx_security_error_cnt;
    r->coex_rx = IEEE802154.debug_rx_abort_coex_cnt;
    r->coex_txack = IEEE802154.debug_tx_ack_abort_coex_cnt;
    r->coex_rxack = IEEE802154.debug_rx_ack_abort_coex_cnt;
    r->coex_tx = IEEE802154.debug_tx_break_coex_cnt;
    r->coex_ed = IEEE802154.debug_ed_scan_break_coex_cnt;
    return true;
}
#endif

void hub_bbox_radio(const char *when)
{
#if CONFIG_SOC_IEEE802154_SUPPORTED
    radio_t r;
    if (!radio_read(&r)) {
        return;
    }
    if (!s_r_have) {                                /* 기준값을 못 잡았으면(스택이 뜨기 전 호출) 지금부터 센다 */
        s_r_boot = s_r_last = r;
        s_r_have = true;
    }
    radio_t p = s_r_last;
    s_r_last = r;
    hub_bbox_note("radio[%s] ack_to+%lu cca_busy+%lu cca_fail+%lu crc+%lu sfd_to+%lu filt+%lu no_rss+%lu rx_restart+%lu "
                  "sec_err+%lu coex(rx+%lu txack+%lu rxack+%lu tx+%lu ed+%lu)", when,
                  (unsigned long)(r.ack_to - p.ack_to), (unsigned long)(r.cca_busy - p.cca_busy),
                  (unsigned long)(r.cca_fail - p.cca_fail), (unsigned long)(r.crc - p.crc),
                  (unsigned long)(r.sfd_to - p.sfd_to), (unsigned long)(r.filt - p.filt),
                  (unsigned long)(r.no_rss - p.no_rss), (unsigned long)(r.rx_restart - p.rx_restart),
                  (unsigned long)(r.sec_err - p.sec_err), (unsigned long)(r.coex_rx - p.coex_rx),
                  (unsigned long)(r.coex_txack - p.coex_txack), (unsigned long)(r.coex_rxack - p.coex_rxack),
                  (unsigned long)(r.coex_tx - p.coex_tx), (unsigned long)(r.coex_ed - p.coex_ed));
#else
    (void)when;
#endif
}

static uint16_t clamp16(uint32_t v) { return v > 0xffff ? 0xffff : (uint16_t)v; }

/* 이력에 넣을 값: 부팅 뒤 지금까지 늘어난 카운터 */
static void radio_into(rec_t *e)
{
#if CONFIG_SOC_IEEE802154_SUPPORTED
    radio_t r;
    if (!radio_read(&r)) {
        return;
    }
    e->ack_to = clamp16(r.ack_to - s_r_boot.ack_to);
    e->cca_busy = clamp16(r.cca_busy - s_r_boot.cca_busy);
    e->cca_fail = clamp16(r.cca_fail - s_r_boot.cca_fail);
    e->crc = clamp16(r.crc - s_r_boot.crc);
#else
    (void)e;
#endif
}

/* ---------------------------------------------------------------- 스캔 시험 */

/* 재가입은 "스캔에서 부모(코디네이터)의 비콘을 잡느냐"에서 갈린다(2026-11-21 기록: 실패한 시도는 재가입 요청을 보내기도
 * 전에 0.17초 만에 NWK 0xc3 = 붙을 부모 없음). 허브가 직접 스캔해 들리는 비콘을 적는다:
 *   - 가입 실패 직후 자동으로 몇 번(그 순간 비콘이 안 들리는지, 들리는데 '단말 받을 자리 없음'인지)
 *   - GET /api/v1/zbscan?n=.. 로 붙어 있는 동안에도(재부팅 없이 수신률을 잰다) */
#define PROBE_TXT 3072
typedef struct {
    uint8_t n, dur, alt, pause;     /* 횟수, 스캔 시간 지수, 홀수 번째는 dur+2 로, Wi-Fi 를 멈추고 */
    uint16_t gap_ms;
    uint8_t quiet;                  /* 주기 측정: 결과 글을 건드리지 않고 횟수만 센다(놓쳤을 때만 한 줄 남김) */
    uint8_t ed;                     /* 채널 소음(에너지) 스캔도 한다. 기본은 안 함: 단말로 붙어 있는 동안 요청하면 끝났다는
                                     * 통지가 오지 않고, 그 뒤의 스캔 요청이 모두 INV_STATE 로 거절됐다(2026-11-21 실측) */
} probe_req_t;
static char s_probe_txt[PROBE_TXT];
static volatile bool s_probe_running;
static SemaphoreHandle_t s_scan_done;
static probe_req_t s_probe_req;
static char s_scan_line[220];
static volatile int s_scan_n, s_scan_ours;
static uint16_t s_net_pan = 0xffff;     /* 마지막으로 붙어 있던 망(NVS "bbox"/"net") */
static uint8_t s_net_ch;
static uint8_t s_scandur = 0xff;        /* BDB 스캔 시간 지수 설정(0xff = 스택 기본값 그대로) */
static uint8_t s_scan_on_fail = 1;
/* 주기 측정(붙어 있는 동안 s_auto_s 초마다 스캔 한 번): 비콘 수신률이 시간대에 따라 어떻게 변하나. 0 = 끔 */
static uint16_t s_auto_s;
static uint32_t s_auto_total, s_auto_hit, s_auto_err;
/* 주기 재부팅(재가입 실패를 다시 잡으려는 시험): s_rb_min 분마다 일부러 재시작한다. 0 = 끔.
 * 붙어 있고 이번 부팅에 실패가 한 번도 없을 때만 한다 — 실패가 잡힌 부팅(되살리기로 올라온 부팅 포함)은 그대로 둬서
 * 직전 부팅 기록이 덮이지 않게 하고, 붙지 못한 채 재시작해 되살리기 횟수를 쓰는 일도 없게 한다. */
static uint16_t s_rb_min;
static uint32_t s_rb_from;                  /* 켠 때의 부팅 번호(그 뒤로 몇 번 재부팅했나 세려고) */
static bool s_rb_resume;                    /* 실패가 잡혀 멈춘 부팅에서 "그래도 계속" 을 받았나 */
static int8_t s_auto_rssi, s_auto_ed;       /* 마지막으로 들은 우리 망 비콘의 신호 / 우리 채널 소음 */
static int8_t s_scan_rssi, s_scan_ed;       /* 스캔 한 번의 값(콜백이 채움) */
/* 가입 최소 신호 품질(LQI): 스택은 비콘의 품질이 이 값보다 낮은 부모는 고르지 않는다. 0xff = 스택 기본값 그대로.
 * 스택 기본값은 75 인데, 허브와 SmartThings 허브가 방 하나쯤 떨어지면 비콘 품질이 30~80 을 오간다. 그러면 부팅 때
 * 재가입이 "붙을 부모 없음(0x2c3)" 으로 연달아 실패하고, 끝내 가입 정보를 잃는다 — 비콘은 다 들리는데도(2026-11-22
 * 11:01 의 실패: 직후 스캔 8번 중 8번 들림, 품질 31~77). 40 으로 낮춘 뒤로는 재부팅마다 1초에 붙었다.
 * 40 인 까닭: 코디네이터는 거의 늘 통과하고, 훨씬 약한 라우터(품질 12~30)는 부모로 고르지 않게. */
#define MIN_JOIN_LQI_DEFAULT 40
static uint8_t s_minlqi = MIN_JOIN_LQI_DEFAULT;
static uint8_t s_minlqi_boot = 0xff;        /* 이번 부팅에 스택이 들고 있던 값(우리가 바꾸기 전) */
static uint8_t s_scan_lqa;                  /* 스캔 한 번에서 우리 망 비콘 가운데 가장 좋은 품질 */
static uint8_t s_auto_lqa;                  /* 주기 측정의 마지막 값 */
static uint32_t s_lqa_hist[8];              /* 주기 측정에서 본 품질 분포(32 단위 8칸) */

static void probe_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void probe_out(const char *fmt, ...)
{
    char l[236];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l, sizeof(l), fmt, ap);
    va_end(ap);
    hub_bbox_note("%s", l);
    xSemaphoreTake(s_hmx, portMAX_DELAY);
    strlcat(s_probe_txt, l, sizeof(s_probe_txt));
    strlcat(s_probe_txt, "\n", sizeof(s_probe_txt));
    xSemaphoreGive(s_hmx);
}

static void scan_active_cb(ezb_nwk_active_scan_result_t *r, void *ctx)
{
    (void)ctx;
    if (!r) {
        xSemaphoreGive(s_scan_done);
        return;
    }
    s_scan_n++;
    if (r->panid == s_net_pan) {
        s_scan_ours++;
        s_scan_rssi = r->rssi;
        if (r->lqa > s_scan_lqa) {
            s_scan_lqa = r->lqa;
        }
    }
    size_t o = strlen(s_scan_line);
    snprintf(s_scan_line + o, sizeof(s_scan_line) - o, " [%04x pan=%04x ch=%d lqa=%d rssi=%d upd=%d pj=%d rc=%d edc=%d]",
             (unsigned)r->shortaddr, (unsigned)r->panid, r->channel_number, r->lqa, r->rssi, r->nwk_update_id,
             r->permit_join, r->router_capacity, r->enddev_capacity);
}

static void scan_ed_cb(ezb_nwk_ed_scan_result_t *r, void *ctx)
{
    (void)ctx;
    if (!r) {
        xSemaphoreGive(s_scan_done);
        return;
    }
    size_t o = strlen(s_scan_line);
    snprintf(s_scan_line + o, sizeof(s_scan_line) - o, " ch%d=%d", r->channel_number, r->max_rssi);
    s_scan_ed = r->max_rssi;
}

/* 스캔 한 번. 0 = 끝남, -1 = 끝났다는 신호가 안 옴, 그 밖의 음수 = 스택이 거절(오류 코드의 음수) */
static int do_scan(uint8_t type, uint8_t dur, uint32_t mask)
{
    static ezb_nwk_scan_req_t q;
    memset(&q, 0, sizeof(q));
    q.scan_type = type;
    q.scan_duration = dur;
    q.scan_channels = mask;
    if (type == EZB_NWK_SCAN_TYPE_ACTIVE) {
        q.active_scan_cb = scan_active_cb;
    } else {
        q.ed_scan_cb = scan_ed_cb;
    }
    s_scan_line[0] = 0;
    s_scan_n = s_scan_ours = 0;
    s_scan_lqa = 0;
    xSemaphoreTake(s_scan_done, 0);
    esp_zigbee_lock_acquire(portMAX_DELAY);
    ezb_err_t e = ezb_nwk_scan(&q);
    esp_zigbee_lock_release();
    if (e != EZB_ERR_NONE) {
        return -(int)e - 1;
    }
    return xSemaphoreTake(s_scan_done, pdMS_TO_TICKS(8000)) == pdTRUE ? 0 : -1;
}

static void probe_task(void *arg)
{
    (void)arg;
    probe_req_t q = s_probe_req;
    uint8_t ch = s_net_ch >= 11 && s_net_ch <= 26 ? s_net_ch : 20;
    bool paused_here = false;
    if (q.quiet) {                                  /* 주기 측정: 스캔 한 번, 횟수만 */
        int rc = do_scan(EZB_NWK_SCAN_TYPE_ACTIVE, q.dur, 1u << ch);
        if (rc == 0) {
            s_auto_total++;
            if (s_scan_ours) {
                s_auto_hit++;
                s_auto_rssi = s_scan_rssi;
                s_auto_lqa = s_scan_lqa;
                s_lqa_hist[s_scan_lqa >> 5]++;
            } else {
                hub_bbox_note("auto scan miss: dur=%d beacons=%d%s (hit %lu/%lu)", q.dur, s_scan_n, s_scan_line,
                              (unsigned long)s_auto_hit, (unsigned long)s_auto_total);
            }
        } else {
            s_auto_err++;
        }
        s_probe_running = false;
        vTaskDelete(NULL);
        return;
    }
    xSemaphoreTake(s_hmx, portMAX_DELAY);
    s_probe_txt[0] = 0;
    xSemaphoreGive(s_hmx);
    probe_out("scan probe: n=%d dur=%d alt=%d pause=%d gap=%d ch=%d pan=%04x up=%lus wifi_paused=%d", q.n, q.dur, q.alt,
              q.pause, q.gap_ms, ch, s_net_pan, (unsigned long)up_s(), hub_wifi_radio_paused());
    if (q.pause && !hub_wifi_radio_paused()) {
        hub_wifi_radio_pause();
        paused_here = true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    int rc, hit = 0, done = 0;
    if (q.ed) {                                     /* 채널 소음: 우리 채널과, 비교용으로 Wi-Fi 대역 안팎의 채널 몇 개 */
        rc = do_scan(EZB_NWK_SCAN_TYPE_ED, 3, (1u << ch) | (1u << 15) | (1u << 22) | (1u << 25));
        probe_out("ed rc=%d%s", rc, s_scan_line);
        if (rc == 0) {
            s_auto_ed = s_scan_ed;
        }
    }
    for (int i = 0; i < q.n; i++) {
        uint8_t d = (uint8_t)(q.dur + ((q.alt && (i & 1)) ? 2 : 0));
        int64_t t0 = esp_timer_get_time();
        rc = do_scan(EZB_NWK_SCAN_TYPE_ACTIVE, d, 1u << ch);
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        if (rc == 0) {
            done++;
            if (s_scan_ours) {
                hit++;
            }
        }
        probe_out("scan %d dur=%d rc=%d %dms beacons=%d ours=%d%s", i + 1, d, rc, ms, s_scan_n, s_scan_ours, s_scan_line);
        vTaskDelay(pdMS_TO_TICKS(q.gap_ms));
    }
    probe_out("result: our network heard in %d of %d scans", hit, done);
    hub_bbox_radio("probe");
    if (paused_here) {
        hub_wifi_radio_resume();
    }
    s_probe_running = false;
    vTaskDelete(NULL);
}

static bool probe_start(probe_req_t q)
{
    if (s_probe_running || !s_scan_done || !s_stack_up) {
        return false;
    }
    s_probe_running = true;
    s_probe_req = q;
    if (xTaskCreate(probe_task, "bbox_scan", 4096, NULL, 3, NULL) != pdPASS) {
        s_probe_running = false;
        return false;
    }
    return true;
}

static void bbox_set_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("bbox", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void hub_bbox_scan_stats(uint32_t *total, uint32_t *hit, int *rssi, int *ed)
{
    if (total) *total = s_auto_total;
    if (hit) *hit = s_auto_hit;
    if (rssi) *rssi = s_auto_rssi;
    if (ed) *ed = s_auto_ed;
}

esp_err_t hub_bbox_scan_http(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char q[96] = "", v[8];
    int n = 0, dur = 3, gap = 300, pause = 0, alt = 0, ed = 0;
    bool started = false;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) n = atoi(v);
        if (httpd_query_key_value(q, "dur", v, sizeof(v)) == ESP_OK) dur = atoi(v);
        if (httpd_query_key_value(q, "gap", v, sizeof(v)) == ESP_OK) gap = atoi(v);
        if (httpd_query_key_value(q, "pause", v, sizeof(v)) == ESP_OK) pause = atoi(v);
        if (httpd_query_key_value(q, "alt", v, sizeof(v)) == ESP_OK) alt = atoi(v);
        if (httpd_query_key_value(q, "ed", v, sizeof(v)) == ESP_OK) ed = atoi(v);
        if (httpd_query_key_value(q, "setdur", v, sizeof(v)) == ESP_OK) {      /* 스택이 가입·재가입 때 쓰는 스캔 시간 */
            int d = atoi(v);
            if ((d >= 0 && d <= 8) || d == 255) {
                s_scandur = (uint8_t)d;
                bbox_set_u8("scandur", s_scandur);
                if (d != 255 && hub_zigbee_is_ready()) {
                    esp_zigbee_lock_acquire(portMAX_DELAY);
                    ezb_bdb_set_scan_duration((uint8_t)d);
                    esp_zigbee_lock_release();
                }
            }
        }
        if (httpd_query_key_value(q, "minlqi", v, sizeof(v)) == ESP_OK) {      /* 가입 최소 신호 품질(0~254, 255 = 스택 기본값) */
            int m = atoi(v);
            if (m >= 0 && m <= 255) {
                s_minlqi = (uint8_t)m;
                bbox_set_u8("minlqi", s_minlqi);
                if (hub_zigbee_is_ready()) {        /* 지금도 바로 적용(255 면 이번 부팅의 스택 값으로 되돌림) */
                    esp_zigbee_lock_acquire(portMAX_DELAY);
                    ezb_nwk_set_min_join_lqi(s_minlqi != 0xff ? s_minlqi : s_minlqi_boot);
                    esp_zigbee_lock_release();
                }
                hub_bbox_note("min_join_lqi set: %d", s_minlqi);
            }
        }
        if (httpd_query_key_value(q, "onfail", v, sizeof(v)) == ESP_OK) {
            s_scan_on_fail = atoi(v) ? 1 : 0;
            bbox_set_u8("scanfail", s_scan_on_fail);
        }
        if (httpd_query_key_value(q, "auto", v, sizeof(v)) == ESP_OK) {        /* 주기 측정 간격(초, 0 = 끔, 최소 30) */
            int a = atoi(v);
            s_auto_s = (uint16_t)(a <= 0 ? 0 : a < 30 ? 30 : a > 3600 ? 3600 : a);
            nvs_handle_t h;
            if (nvs_open("bbox", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u16(h, "scanauto", s_auto_s);
                nvs_commit(h);
                nvs_close(h);
            }
        }
    }
    if (n > 0) {
        probe_req_t r = {
            .n = (uint8_t)(n > 40 ? 40 : n), .dur = (uint8_t)(dur < 0 ? 0 : dur > 8 ? 8 : dur), .alt = alt ? 1 : 0,
            .pause = pause ? 1 : 0, .gap_ms = (uint16_t)(gap < 0 ? 0 : gap > 5000 ? 5000 : gap), .ed = ed ? 1 : 0,
        };
        started = probe_start(r);
    }
    int bdb_dur = -1, minlqi = -1;
    if (hub_zigbee_is_ready()) {
        esp_zigbee_lock_acquire(portMAX_DELAY);
        bdb_dur = ezb_bdb_get_scan_duration();
        minlqi = ezb_nwk_get_min_join_lqi();
        esp_zigbee_lock_release();
    }
    char hdr[520];
    int hl = snprintf(hdr, sizeof(hdr), "# zbscan: running=%d started=%d bdb_scan_dur=%d setdur=%d onfail=%d net=pan %04x ch %d\n"
             "# min_join_lqi: now=%d stack_default=%d set=%d\n"
             "# auto: every %us, hit %lu of %lu (err %lu), last rssi %d lqa %d, noise %d\n# auto lqa histogram:",
             s_probe_running, started, bdb_dur, s_scandur, s_scan_on_fail, s_net_pan, s_net_ch, minlqi, s_minlqi_boot, s_minlqi,
             s_auto_s, (unsigned long)s_auto_hit, (unsigned long)s_auto_total, (unsigned long)s_auto_err, s_auto_rssi,
             s_auto_lqa, s_auto_ed);
    for (int i = 0; i < 8 && hl < (int)sizeof(hdr) - 24; i++) {
        hl += snprintf(hdr + hl, sizeof(hdr) - (size_t)hl, " %d-%d:%lu", i * 32, i * 32 + 31, (unsigned long)s_lqa_hist[i]);
    }
    snprintf(hdr + hl, sizeof(hdr) - (size_t)hl, "\n");
    httpd_resp_sendstr_chunk(req, hdr);
    char *copy = malloc(PROBE_TXT);
    if (copy) {
        xSemaphoreTake(s_hmx, portMAX_DELAY);
        strlcpy(copy, s_probe_txt, PROBE_TXT);
        xSemaphoreGive(s_hmx);
        if (copy[0]) {
            httpd_resp_sendstr_chunk(req, copy);
        }
        free(copy);
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---------------------------------------------------------------- 부팅 이력 */

static void hist_save(void)
{
    nvs_handle_t h;
    if (nvs_open("bbox", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "hist", s_hist, sizeof(rec_t) * (size_t)s_hn);
        nvs_commit(h);
        nvs_close(h);
    }
}

static rec_t *cur_rec(void) { return s_started && s_hn > 0 ? &s_hist[s_hn - 1] : NULL; }

void hub_bbox_zb_boot(uint32_t fc)
{
    if (s_net_pan == 0xffff && !ezb_bdb_is_factory_new()) {   /* 아직 적어 둔 게 없으면 저장된 가입 정보에서 */
        s_net_pan = ezb_nwk_get_panid();
        s_net_ch = ezb_nwk_get_current_channel();
    }
#if CONFIG_SOC_IEEE802154_SUPPORTED
    s_stack_up = true;
    if (radio_read(&s_r_boot)) {                    /* 이 부팅의 기준값. 재시작 때 0 으로 돌아가는지도 기록으로 확인한다 */
        s_r_last = s_r_boot;
        s_r_have = true;
        hub_bbox_note("radio base: ack_to=%lu cca_busy=%lu cca_fail=%lu crc=%lu sfd_to=%lu filt=%lu",
                      (unsigned long)s_r_boot.ack_to, (unsigned long)s_r_boot.cca_busy, (unsigned long)s_r_boot.cca_fail,
                      (unsigned long)s_r_boot.crc, (unsigned long)s_r_boot.sfd_to, (unsigned long)s_r_boot.filt);
    }
#endif
    hub_bbox_note("zb boot: fc=%lu prev_fc=%lu(%s, up %lus) bdb_scan_dur=%d setdur=%d", (unsigned long)fc,
                  (unsigned long)s_prev_fc,
                  cur_rec() ? (cur_rec()->fc_src == 1 ? "ram" : cur_rec()->fc_src == 2 ? "flash" : "none") : "?",
                  (unsigned long)s_prev_fc_up, ezb_bdb_get_scan_duration(), s_scandur);
    if (s_scandur != 0xff) {                        /* 시험용 설정: 가입·재가입 스캔을 더 오래(여기는 스택 문맥 — 잠금 불필요) */
        ezb_bdb_set_scan_duration(s_scandur);
    }
    s_minlqi_boot = ezb_nwk_get_min_join_lqi();
    if (s_minlqi != 0xff) {
        ezb_nwk_set_min_join_lqi(s_minlqi);
    }
    hub_bbox_note("zb boot: min_join_lqi stack=%d set=%d now=%d", s_minlqi_boot, s_minlqi, ezb_nwk_get_min_join_lqi());
    if (!s_hmx) {
        return;
    }
    xSemaphoreTake(s_hmx, portMAX_DELAY);
    rec_t *e = cur_rec();
    if (e && !e->fc_boot) {
        e->fc_boot = fc;
        hist_save();
    }
    xSemaphoreGive(s_hmx);
}

void hub_bbox_zb_fail(uint8_t status)
{
    hub_bbox_radio("fail");
    if (!s_hmx) {
        return;
    }
    xSemaphoreTake(s_hmx, portMAX_DELAY);
    rec_t *e = cur_rec();
    if (e && e->result != 1) {
        if (!e->fails) {
            e->first_fail = status;
        }
        if (e->fails < 0xff) {
            e->fails++;
        }
        if (e->fails <= 8) {                        /* 끝없이 실패할 때 플래시를 계속 쓰지 않는다 */
            radio_into(e);
            hist_save();
        }
    }
    bool probe = e && e->result != 1 && e->fails <= 4;
    xSemaphoreGive(s_hmx);
    /* 실패한 바로 그때 비콘이 들리는지 본다(부팅당 4번까지). Wi-Fi 가 멈춰 있으면 가입 창이 이어지는 중이라 끼어들지 않는다 */
    if (probe && s_scan_on_fail && !hub_wifi_radio_paused()) {
        probe_start((probe_req_t){ .n = 4, .dur = 3, .alt = 1, .pause = 1, .gap_ms = 200 });
    }
}

void hub_bbox_zb_joined(void)
{
    hub_bbox_radio("joined");
    {
        uint16_t pan = ezb_nwk_get_panid();         /* 스택 문맥(신호 처리 안)에서 불린다 */
        uint8_t ch = ezb_nwk_get_current_channel();
        if (pan != s_net_pan || ch != s_net_ch) {
            s_net_pan = pan;
            s_net_ch = ch;
            nvs_handle_t h;
            if (nvs_open("bbox", NVS_READWRITE, &h) == ESP_OK) {
                uint8_t b[3] = { (uint8_t)pan, (uint8_t)(pan >> 8), ch };
                nvs_set_blob(h, "net", b, sizeof(b));
                nvs_commit(h);
                nvs_close(h);
            }
        }
    }
    if (!s_hmx) {
        return;
    }
    xSemaphoreTake(s_hmx, portMAX_DELAY);
    rec_t *e = cur_rec();
    bool after_fail = false;
    if (e && e->result != 1) {
        e->result = 1;
        e->join_s = clamp16(up_s());
        radio_into(e);
        hist_save();
        after_fail = e->fails > 0;
    }
    xSemaphoreGive(s_hmx);
    if (after_fail) {                               /* 실패 끝에 붙었다 — 붙은 순간의 비콘 품질을 실패 때와 견줄 수 있게 */
        probe_start((probe_req_t){ .n = 4, .dur = 3, .gap_ms = 200 });
    }
}

void hub_bbox_zb_left(uint8_t leave_type, bool while_up)
{
    hub_bbox_radio("left");
    if (!s_hmx) {
        return;
    }
    xSemaphoreTake(s_hmx, portMAX_DELAY);
    rec_t *e = cur_rec();
    if (e) {
        e->result = while_up ? 3 : 2;
        e->leave_type = leave_type;
        e->leave_s = clamp16(up_s());
        if (!while_up) {
            radio_into(e);
        }
        hist_save();
    }
    xSemaphoreGive(s_hmx);
}

/* ---------------------------------------------------------------- 프레임 카운터 */

static void fc_sample(void)
{
    if (!hub_zigbee_is_ready()) {
        return;
    }
    esp_zigbee_lock_acquire(portMAX_DELAY);
    uint32_t fc = ezb_nwk_get_frame_counter();
    esp_zigbee_lock_release();
    if (fc >= s_nv.fc_last) {                       /* 망을 떠나면 0 으로 돌아간다 — 줄어든 값은 버린다(직전 최댓값 유지) */
        s_nv.fc_last = fc;
        s_nv.fc_up_s = up_s();
    }
}

void hub_bbox_before_restart(void)
{
    if (!s_started) {
        return;
    }
    fc_sample();
    last_t l = { .fc = s_nv.fc_last, .up_s = up_s(), .boot_no = s_hn ? s_hist[s_hn - 1].boot_no : 0 };
    nvs_handle_t h;
    if (nvs_open("bbox", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "last", &l, sizeof(l));
        nvs_commit(h);
        nvs_close(h);
    }
    hub_bbox_note("restart: fc_last=%lu", (unsigned long)l.fc);
}

/* 이번 부팅에 실패 흔적이 없나(있으면 주기 재부팅을 멈춘다) */
static bool rb_clean(const rec_t *e) { return !e->fails && !e->restore_try; }

static void fc_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FC_TICK_MS));
        fc_sample();
        {
            static uint32_t last_auto;
            rec_t *r = cur_rec();
            /* 붙어 있고 Wi-Fi 가 멈춰 있지 않을 때만(가입 시도 중에는 끼어들지 않는다) */
            if (s_auto_s && r && r->result == 1 && !hub_wifi_radio_paused() && up_s() - last_auto >= s_auto_s) {
                last_auto = up_s();
                probe_start((probe_req_t){ .n = 1, .dur = 3, .quiet = 1 });
            }
        }
        xSemaphoreTake(s_hmx, portMAX_DELAY);
        rec_t *e = cur_rec();
        time_t now = time(NULL);
        if (e && !e->epoch && now > 1700000000) {   /* 시계가 맞으면 부팅 시각을 채운다 */
            e->epoch = (uint32_t)(now - (time_t)up_s());
            hist_save();
        }
        bool rb = s_rb_min && e && e->result == 1 && (rb_clean(e) || s_rb_resume) && !s_probe_running &&
                  up_s() >= (uint32_t)s_rb_min * 60;
        xSemaphoreGive(s_hmx);
        if (rb) {
            hub_bbox_note("periodic reboot: up %lus (every %u min)", (unsigned long)up_s(), s_rb_min);
            ESP_LOGW(TAG, "주기 재부팅(%u분마다)", s_rb_min);
            hub_ota_planned_reboot();
            esp_restart();
        }
    }
}

/* ---------------------------------------------------------------- 시작 */

void hub_bbox_init(void)
{
    bool ok = s_nv.magic == MAGIC && s_nv.magic2 == MAGIC && s_nv.cur <= 1 && s_nv.s[0].zhead_len <= ZHEAD_CAP &&
              s_nv.s[1].zhead_len <= ZHEAD_CAP;
    if (ok) {
        s_prev_ok = true;
        s_prev_fc = s_nv.fc_last;
        s_prev_fc_up = s_nv.fc_up_s;
        s_nv.cur ^= 1;
    } else {
        memset(&s_nv, 0, sizeof(s_nv));
        s_nv.magic = s_nv.magic2 = MAGIC;
    }
    slot_t *c = &s_nv.s[s_nv.cur];
    c->boot_no = 0;
    c->zhead_len = 0;
    c->ztail_total = 0;
    c->gtail_total = 0;
    s_nv.fc_last = 0;
    s_nv.fc_up_s = 0;
    s_mx = xSemaphoreCreateMutex();
    s_hmx = xSemaphoreCreateMutex();
    s_scan_done = xSemaphoreCreateBinary();
    s_prev_vprintf = esp_log_set_vprintf(bbox_vprintf);
}

void hub_bbox_start(void)
{
    if (!s_hmx) {
        return;
    }
    nvs_handle_t h;
    uint32_t seq = 0;
    last_t l = { 0 };
    bool have_last = false;
    uint8_t rjtry = 0;
    if (nvs_open("zbmark", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "rjtry", &rjtry);
        nvs_close(h);
    }
    if (nvs_open("bbox", NVS_READWRITE, &h) == ESP_OK) {
        size_t sz = sizeof(s_hist);
        if (nvs_get_blob(h, "hist", s_hist, &sz) == ESP_OK && sz % sizeof(rec_t) == 0) {
            s_hn = (int)(sz / sizeof(rec_t));
        } else {
            s_hn = 0;
        }
        nvs_get_u32(h, "seq", &seq);
        seq++;
        nvs_set_u32(h, "seq", seq);
        sz = sizeof(l);
        have_last = nvs_get_blob(h, "last", &l, &sz) == ESP_OK && sz == sizeof(l);
        if (have_last) {
            nvs_erase_key(h, "last");               /* 한 번만 쓴다 — 다음 부팅이 오래된 값을 보지 않게 */
        }
        uint8_t lvl, b[3];
        if (nvs_get_u8(h, "zblvl", &lvl) == ESP_OK && lvl <= EZB_LOG_LEVEL_VERBOSE) {
            s_zb_lvl = lvl;
        }
        nvs_get_u8(h, "scandur", &s_scandur);
        nvs_get_u8(h, "minlqi", &s_minlqi);
        nvs_get_u8(h, "scanfail", &s_scan_on_fail);
        nvs_get_u16(h, "scanauto", &s_auto_s);
        nvs_get_u16(h, "rbmin", &s_rb_min);
        nvs_get_u32(h, "rbfrom", &s_rb_from);
        sz = sizeof(b);
        if (nvs_get_blob(h, "net", b, &sz) == ESP_OK && sz == sizeof(b)) {
            s_net_pan = (uint16_t)(b[0] | (b[1] << 8));
            s_net_ch = b[2];
        }
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreTake(s_hmx, portMAX_DELAY);
    if (s_hn >= HIST_N) {
        memmove(s_hist, s_hist + 1, sizeof(rec_t) * (HIST_N - 1));
        s_hn = HIST_N - 1;
    }
    rec_t *e = &s_hist[s_hn++];
    memset(e, 0, sizeof(*e));
    e->boot_no = seq;
    e->reset_reason = (uint8_t)esp_reset_reason();
    const esp_partition_t *run = esp_ota_get_running_partition();
    e->slot = run && run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1 ? 1 : 0;
    e->restore_try = rjtry;
    e->join_s = 0xffff;
    if (s_prev_ok && s_prev_fc) {                   /* 같은 빌드의 재시작 — RAM 에 남은 값이 가장 새것 */
        e->fc_prev = s_prev_fc;
        e->fc_src = 1;
    } else if (have_last) {                         /* 다른 빌드로 넘어온 OTA 등 — 재시작 직전에 적어 둔 값 */
        e->fc_prev = s_prev_fc = l.fc;
        s_prev_fc_up = l.up_s;
        e->fc_src = 2;
    }
    s_nv.s[s_nv.cur].boot_no = seq;
    s_started = true;
    hist_save();
    xSemaphoreGive(s_hmx);
    ESP_LOGI(TAG, "부팅 #%lu (사유 %d, 되살리기 %d회째) — 직전 부팅 기록 %s", (unsigned long)seq, e->reset_reason, rjtry,
             s_prev_ok ? "있음" : "없음");
    xTaskCreate(fc_task, "bbox_fc", 3072, NULL, 2, NULL);
}

/* ---------------------------------------------------------------- HTTP */

static const char *reset_name(uint8_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power";
    case ESP_RST_SW:       return "restart";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB:      return "usb";
    case ESP_RST_JTAG:     return "jtag";
    case ESP_RST_EXT:      return "ext";
    default:               return "other";
    }
}

static void send_hist(httpd_req_t *req)
{
    char line[320];
    rec_t *copy = malloc(sizeof(s_hist));
    int n = 0;
    if (!copy) {
        return;
    }
    xSemaphoreTake(s_hmx, portMAX_DELAY);
    n = s_hn;
    memcpy(copy, s_hist, sizeof(rec_t) * (size_t)n);
    xSemaphoreGive(s_hmx);
    httpd_resp_sendstr_chunk(req, "# boots (oldest first; last = this boot)\n");
    for (int i = 0; i < n; i++) {
        const rec_t *e = &copy[i];
        char when[24] = "?";
        if (e->epoch) {
            time_t t = (time_t)e->epoch;
            struct tm tm;
            localtime_r(&t, &tm);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        }
        static const char *const RES[] = { "pending", "joined", "lost_before_join", "left_while_up" };
        snprintf(line, sizeof(line),
                 "boot=%lu time=%s reset=%s slot=ota_%d restore_try=%d result=%s join_s=%d fails=%d first_fail=0x%02x "
                 "leave_type=%d leave_s=%d fc_prev=%lu(%s) fc_boot=%lu ack_to=%u cca_busy=%u cca_fail=%u crc=%u\n",
                 (unsigned long)e->boot_no, when, reset_name(e->reset_reason), e->slot, e->restore_try,
                 RES[e->result & 3], e->join_s == 0xffff ? -1 : e->join_s, e->fails, e->first_fail,
                 e->result >= 2 ? e->leave_type : -1, e->result >= 2 ? e->leave_s : -1, (unsigned long)e->fc_prev,
                 e->fc_src == 1 ? "ram" : e->fc_src == 2 ? "flash" : "none", (unsigned long)e->fc_boot, e->ack_to,
                 e->cca_busy, e->cca_fail, e->crc);
        httpd_resp_sendstr_chunk(req, line);
    }
    free(copy);
}

/* 고리를 오래된 것부터 보낸다. 덮어쓴 적이 있으면 맨 앞은 줄 중간일 수 있어 다음 줄부터 보낸다. */
static void send_ring(httpd_req_t *req, const char *buf, int cap, uint32_t total)
{
    if (total <= (uint32_t)cap) {
        if (total) {
            httpd_resp_send_chunk(req, buf, (ssize_t)total);
        }
        return;
    }
    int at = (int)(total % (uint32_t)cap), i = 0;
    while (i < cap && buf[(at + i) % cap] != '\n') {
        i++;
    }
    i++;
    if (i >= cap) {
        return;
    }
    int from = (at + i) % cap, len = cap - i, first = cap - from < len ? cap - from : len;
    httpd_resp_send_chunk(req, buf + from, first);
    if (len > first) {
        httpd_resp_send_chunk(req, buf, len - first);
    }
}

/* 한 칸을 보낸다. live = 이번 부팅(쓰는 중이라 사본을 떠서 보낸다. 메모리가 없으면 그대로 — 조금 섞일 수 있다) */
static void send_slot(httpd_req_t *req, int idx, bool live, const char *title)
{
    char hdr[200];
    const slot_t *s = &s_nv.s[idx];
    slot_t *copy = live ? malloc(sizeof(slot_t)) : NULL;
    if (copy) {
        portENTER_CRITICAL(&s_mux);
        memcpy(copy, s, sizeof(slot_t));
        portEXIT_CRITICAL(&s_mux);
        s = copy;
    }
    snprintf(hdr, sizeof(hdr), "\n# %s: boot=%lu — zigbee lines: first %u bytes, then last %d of %lu bytes\n", title,
             (unsigned long)s->boot_no, (unsigned)s->zhead_len, s->ztail_total > ZTAIL_CAP ? ZTAIL_CAP : (int)s->ztail_total,
             (unsigned long)s->ztail_total);
    httpd_resp_sendstr_chunk(req, hdr);
    if (s->zhead_len) {
        httpd_resp_send_chunk(req, s->zhead, s->zhead_len);
    }
    if (s->ztail_total > ZTAIL_CAP) {
        httpd_resp_sendstr_chunk(req, "\n# ... (skipped) ...\n");
    }
    send_ring(req, s->ztail, ZTAIL_CAP, s->ztail_total);
    snprintf(hdr, sizeof(hdr), "\n# %s: other lines, last %d of %lu bytes\n", title,
             s->gtail_total > GTAIL_CAP ? GTAIL_CAP : (int)s->gtail_total, (unsigned long)s->gtail_total);
    httpd_resp_sendstr_chunk(req, hdr);
    send_ring(req, s->gtail, GTAIL_CAP, s->gtail_total);
    free(copy);
}

/* part=tasks: 힙 요약과 태스크 목록(이름, 스택 최소 여유 바이트, 우선순위). 스택 크기 자체는 FreeRTOS 가 알려 주지 않는다. */
static void send_tasks(httpd_req_t *req)
{
    char line[160];
    snprintf(line, sizeof(line), "== heap: free %lu, min %lu, largest %lu, tasks %u ==\n",
             (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT), (unsigned)uxTaskGetNumberOfTasks());
    httpd_resp_sendstr_chunk(req, line);
#if configUSE_TRACE_FACILITY
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *t = calloc(n, sizeof(*t));
    if (!t) {
        return;
    }
    n = uxTaskGetSystemState(t, n, NULL);
    for (UBaseType_t i = 0; i < n; i++) {
        snprintf(line, sizeof(line), "task %-16s stack_min_free=%-6lu prio=%u\n", t[i].pcTaskName,
                 (unsigned long)t[i].usStackHighWaterMark, (unsigned)t[i].uxCurrentPriority);
        httpd_resp_sendstr_chunk(req, line);
    }
    free(t);
#else
    httpd_resp_sendstr_chunk(req, "(task list needs CONFIG_FREERTOS_USE_TRACE_FACILITY)\n");
#endif
}

esp_err_t hub_bbox_http(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char q[64] = "", part[8] = "", v[8] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        httpd_query_key_value(q, "part", part, sizeof(part));
        if (httpd_query_key_value(q, "zblvl", v, sizeof(v)) == ESP_OK) {
            int lvl = atoi(v);
            if (lvl >= 0 && lvl <= EZB_LOG_LEVEL_VERBOSE) {
                nvs_handle_t h;
                s_zb_lvl = (uint8_t)lvl;
                if (nvs_open("bbox", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_set_u8(h, "zblvl", (uint8_t)lvl);
                    nvs_commit(h);
                    nvs_close(h);
                }
            }
        }
        if (httpd_query_key_value(q, "cloudblock", v, sizeof(v)) == ESP_OK) {  /* 시험: 린나이 서버에 닿지 않는 것처럼(자동 폴백 확인) */
            hub_proxy_test_block(atoi(v));
        }
        if (httpd_query_key_value(q, "reboot", v, sizeof(v)) == ESP_OK) {      /* 주기 재부팅 간격(분, 0 = 끔, 5~1440) */
            int m = atoi(v);
            nvs_handle_t h;
            xSemaphoreTake(s_hmx, portMAX_DELAY);
            rec_t *e = cur_rec();
            if (m > 0 && !s_rb_min && e) {
                s_rb_from = e->boot_no;
            }
            s_rb_min = (uint16_t)(m <= 0 ? 0 : m < 5 ? 5 : m > 1440 ? 1440 : m);
            s_rb_resume = s_rb_min != 0;            /* 멈춰 있던 부팅에서 다시 켜면 이어서 한다 */
            xSemaphoreGive(s_hmx);
            if (nvs_open("bbox", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u16(h, "rbmin", s_rb_min);
                nvs_set_u32(h, "rbfrom", s_rb_from);
                nvs_commit(h);
                nvs_close(h);
            }
            hub_bbox_note("periodic reboot set: every %u min", s_rb_min);
        }
    }
    bool all = !part[0];
    char hdr[224];
    int hl = snprintf(hdr, sizeof(hdr), "# up_s=%lu zblvl=%d prev=%s zb_stack_free=%d\n", (unsigned long)up_s(), s_zb_lvl,
                      s_prev_ok ? "yes" : "no", hub_zigbee_stack_free());
    xSemaphoreTake(s_hmx, portMAX_DELAY);
    rec_t *e = cur_rec();
    if (!s_rb_min) {
        snprintf(hdr + hl, sizeof(hdr) - (size_t)hl, "# periodic reboot: off\n");
    } else if (e) {
        bool held = !(rb_clean(e) || s_rb_resume);
        long left = (long)s_rb_min * 60 - (long)up_s();
        snprintf(hdr + hl, sizeof(hdr) - (size_t)hl, "# periodic reboot: every %u min, %lu boots since on, %s (next in %lds)\n",
                 s_rb_min, (unsigned long)(e->boot_no - s_rb_from),
                 held ? "HELD: this boot saw a join failure" : e->result == 1 ? "armed" : "waiting for join",
                 held ? -1L : left < 0 ? 0L : left);
    }
    xSemaphoreGive(s_hmx);
    httpd_resp_sendstr_chunk(req, hdr);
    if (all || !strcmp(part, "hist")) {
        send_hist(req);
    }
    if ((all || !strcmp(part, "prev")) && s_prev_ok) {
        send_slot(req, s_nv.cur ^ 1, false, "previous boot");
    }
    if (all || !strcmp(part, "cur")) {
        send_slot(req, s_nv.cur, true, "this boot");
    }
    if (!strcmp(part, "tasks")) {                    /* 태스크마다 스택이 가장 적게 남았던 양 — 스택 크기를 근거 있게 잡으려고 */
        send_tasks(req);
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}
