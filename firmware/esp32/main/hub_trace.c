/*
 * hub_trace.c -- 기기↔허브↔클라우드 MQTT PUBLISH 기록(고리 버퍼) + GET /api/v1/trace. 설명은 hub_trace.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "hub_trace.h"
#include "hub_events.h"

static const char *TAG = "HUB_TRACE";

#define TR_CAP      8192        /* 고리 버퍼 바이트 */
#define TR_PAY_MAX  640         /* 메시지 하나에서 보관하는 본문 최대(넘으면 자르고 truncated 표시) */
#define TR_TOPIC_MAX 160
#define TR_SNAP     6144        /* 응답 한 번에 내보내는 기록 바이트 한도 */
#define TR_LINE     2200        /* JSON 한 줄 작업 버퍼 */
#define TR_POLL_S   30          /* hub_rinnai 의 주기 상태 요청 간격(응답 머리에 알려 줌) */

typedef struct __attribute__((packed)) {
    uint16_t size;              /* 이 머리 + 토픽 + 본문 */
    uint32_t seq;
    uint32_t ms_lo;             /* 부팅 후 ms (48비트) */
    uint16_t ms_hi;
    uint8_t  dir, src, kind;
    uint8_t  mac3[3];
    uint8_t  tlen;              /* 토픽 길이 */
    uint16_t plen;              /* 보관한 본문 길이 */
    uint16_t full;              /* 원래 본문 길이 */
} rec_t;

static uint8_t *s_buf;
static int s_start, s_end;      /* [s_start, s_end) 에 기록이 순서대로 붙어 있다 */
static uint32_t s_next = 1;     /* 다음에 줄 순번 */
static SemaphoreHandle_t s_mx;

uint64_t hub_trace_now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

void hub_trace_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_buf = malloc(TR_CAP);
    if (!s_buf || !s_mx) {
        ESP_LOGW(TAG, "기록 버퍼를 못 잡음 — 메시지 기록 끔");
        free(s_buf);
        s_buf = NULL;
    }
}

uint32_t hub_trace_add(uint8_t dir, uint8_t src, uint8_t kind, const uint8_t mac3[3], const uint8_t *pkt, int n, int hlen)
{
    if (!s_buf || !pkt || n < hlen + 2 || (pkt[0] >> 4) != 3) {      /* PUBLISH 만 */
        return 0;
    }
    int qos = (pkt[0] >> 1) & 3;
    int tl = (pkt[hlen] << 8) | pkt[hlen + 1];
    int off = hlen + 2 + tl + (qos ? 2 : 0);
    if (tl <= 0 || tl > TR_TOPIC_MAX || off > n) {
        return 0;
    }
    int full = n - off;
    int keep = full > TR_PAY_MAX ? TR_PAY_MAX : full;
    int need = (int)sizeof(rec_t) + tl + keep;
    uint64_t ms = hub_trace_now_ms();

    xSemaphoreTake(s_mx, portMAX_DELAY);
    while (s_end - s_start + need > TR_CAP && s_start < s_end) {       /* 자리가 날 때까지 오래된 것부터 버림 */
        rec_t r;
        memcpy(&r, s_buf + s_start, sizeof(r));
        s_start += r.size;
    }
    if (s_end + need > TR_CAP) {                                      /* 뒤가 모자라면 앞으로 당긴다 */
        memmove(s_buf, s_buf + s_start, (size_t)(s_end - s_start));
        s_end -= s_start;
        s_start = 0;
    }
    rec_t r = {
        .size = (uint16_t)need, .seq = s_next++, .ms_lo = (uint32_t)ms, .ms_hi = (uint16_t)(ms >> 32),
        .dir = dir, .src = src, .kind = kind, .tlen = (uint8_t)tl, .plen = (uint16_t)keep, .full = (uint16_t)(full > 0xffff ? 0xffff : full),
    };
    if (mac3) {
        memcpy(r.mac3, mac3, 3);
    }
    memcpy(s_buf + s_end, &r, sizeof(r));
    memcpy(s_buf + s_end + sizeof(r), pkt + hlen + 2, (size_t)tl);
    memcpy(s_buf + s_end + sizeof(r) + tl, pkt + off, (size_t)keep);
    s_end += need;
    uint32_t seq = r.seq;
    xSemaphoreGive(s_mx);
    return seq;
}

/* ---------------------------------------------------------------- HTTP */

static const char *dir_name(uint8_t d)
{
    return d == HUB_TR_DEV2CLOUD ? "dev>cloud" : d == HUB_TR_CLOUD2DEV ? "cloud>dev" : "hub>dev";
}

static const char *src_name(uint8_t s)
{
    switch (s) {
    case HUB_TR_SRC_DEVICE:   return "device";
    case HUB_TR_SRC_CLOUD:    return "cloud";
    case HUB_TR_SRC_PLATFORM: return "smartthings";
    case HUB_TR_SRC_POLL:     return "hub_poll";
    case HUB_TR_SRC_TIME:     return "hub_time";
    default:                  return "?";
    }
}

/* JSON 문자열 안에 넣을 수 있게 덧붙인다. 넘치면 false. */
static bool cat_json(char *out, size_t cap, size_t *o, const uint8_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (*o + 7 >= cap) {
            return false;
        }
        if (c == '"' || c == '\\') {
            out[(*o)++] = '\\';
            out[(*o)++] = (char)c;
        } else if (c < 0x20) {
            *o += (size_t)snprintf(out + *o, cap - *o, "\\u%04x", c);
        } else {
            out[(*o)++] = (char)c;
        }
    }
    out[*o] = 0;
    return true;
}

static bool printable(const uint8_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] >= 0x7f) {
            return false;
        }
    }
    return true;
}

esp_err_t hub_trace_http(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    uint32_t since = 0;
    int max = 20;
    char q[48], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
            since = (uint32_t)strtoul(v, NULL, 10);
        }
        if (httpd_query_key_value(q, "max", v, sizeof(v)) == ESP_OK) {
            max = atoi(v);
        }
    }
    if (max < 1) {
        max = 1;
    } else if (max > 50) {
        max = 50;
    }
    char *work = malloc(TR_SNAP + TR_LINE);
    if (!work || !s_buf) {
        free(work);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"trace off (no memory)\"}");
    }
    uint8_t *snap = (uint8_t *)work;
    char *line = work + TR_SNAP;

    /* 잠금 안에서는 복사만 한다(소켓으로 보내는 동안 링크 태스크를 붙잡지 않게) */
    int sn = 0, cnt = 0;
    bool full = false;
    uint32_t first = 0, newest = 0, last = since;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    newest = s_next - 1;
    for (int p = s_start; p < s_end;) {
        rec_t r;
        memcpy(&r, s_buf + p, sizeof(r));
        if (!first) {
            first = r.seq;
        }
        if (r.seq > since && !full) {
            if (cnt < max && sn + r.size <= TR_SNAP) {
                memcpy(snap + sn, s_buf + p, r.size);
                sn += r.size;
                cnt++;
                last = r.seq;
            } else {
                full = true;                      /* 여기서 끊는다 — 건너뛰고 뒤의 것을 주면 순번에 구멍이 난다 */
            }
        }
        p += r.size;
    }
    xSemaphoreGive(s_mx);
    if (!first) {
        first = newest + 1;                       /* 비어 있음 */
    }
    if (since > newest) {
        last = newest;                            /* 허브가 다시 켜져 순번이 처음부터 — boot_id 가 바뀌었을 것 */
    }
    uint32_t lost = (since && since + 1 < first) ? first - (since + 1) : 0;

    uint64_t up = hub_trace_now_ms();
    time_t now = time(NULL);
    bool synced = now > 1700000000;
    char tms[24] = "null";
    if (synced) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        snprintf(tms, sizeof(tms), "%lld", (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000);
    }
    snprintf(line, TR_LINE,
             "{\"v\":1,\"boot_id\":%lu,\"up_ms\":%llu,\"time_ms\":%s,\"clock_synced\":%s,\"poll_s\":%d,"
             "\"first\":%lu,\"newest\":%lu,\"lost\":%lu,\"next\":%lu,\"msgs\":[",
             (unsigned long)hub_events_boot_id(), (unsigned long long)up, tms, synced ? "true" : "false", TR_POLL_S,
             (unsigned long)first, (unsigned long)newest, (unsigned long)lost, (unsigned long)last);
    httpd_resp_sendstr_chunk(req, line);

    bool sep = false;
    for (int p = 0; p < sn;) {
        rec_t r;
        memcpy(&r, snap + p, sizeof(r));
        const uint8_t *topic = snap + p + sizeof(r), *pay = topic + r.tlen;
        uint64_t ms = ((uint64_t)r.ms_hi << 32) | r.ms_lo;
        size_t o = (size_t)snprintf(line, TR_LINE,
                                    "%s{\"seq\":%lu,\"up_ms\":%llu,\"dir\":\"%s\",\"src\":\"%s\",\"kind\":\"%s\","
                                    "\"mac\":\"%02x%02x%02x\",\"len\":%u,\"truncated\":%s,\"topic\":\"",
                                    sep ? "," : "", (unsigned long)r.seq, (unsigned long long)ms, dir_name(r.dir), src_name(r.src),
                                    r.kind == 6 ? "f06" : r.kind == 9 ? "f09" : "?", r.mac3[0], r.mac3[1], r.mac3[2],
                                    (unsigned)r.full, r.plen < r.full ? "true" : "false");
        bool ok = cat_json(line, TR_LINE, &o, topic, r.tlen);
        if (ok && printable(pay, r.plen)) {       /* 본문은 보통 JSON 글자 — 그대로(이스케이프) */
            ok = o + 14 < TR_LINE;
            if (ok) {
                o += (size_t)snprintf(line + o, TR_LINE - o, "\",\"payload\":\"");
                ok = cat_json(line, TR_LINE, &o, pay, r.plen);
            }
        } else if (ok) {                          /* 글자가 아니면 16진 */
            ok = o + 10 + (size_t)r.plen * 2 + 4 < TR_LINE;
            if (ok) {
                o += (size_t)snprintf(line + o, TR_LINE - o, "\",\"hex\":\"");
                for (int i = 0; i < r.plen; i++) {
                    o += (size_t)snprintf(line + o, TR_LINE - o, "%02x", pay[i]);
                }
            }
        }
        if (ok && o + 3 < TR_LINE) {
            line[o++] = '"';
            line[o++] = '}';
            line[o] = 0;
            httpd_resp_sendstr_chunk(req, line);
            sep = true;
        }
        p += r.size;
    }
    free(work);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}
