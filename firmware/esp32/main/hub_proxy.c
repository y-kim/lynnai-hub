/*
 * hub_proxy.c -- 내 보일러(와 각방 모듈)를 허브가 받는다. 동작 모드(hub_proxy.h)에 따라
 *   프록시 모드: 제조사 클라우드로 그대로 이어준다(Pi 판 rkprobe/bridge.py). 공식 앱은 그대로 동작한다.
 *   단독 모드  : 클라우드에 안 붙고 허브가 MQTT 상대가 된다(standalone/broker.py 와 같은 최소 응답).
 * /key/server 는 Pi 판 fakesrv.py 처럼 클라우드에 다시 묻는다(못 하면 저장해 둔 마지막 답).
 *
 *   보일러 --TLS1.1(우리 인증서)--> 허브:8883 --TLS1.2/1.3--> 클라우드:8883
 *   보일러 --HTTP GET /key/server--> 허브:80  --HTTP-->        클라우드:80   (접속 정보 XML)
 *
 * 보일러 모듈은 TLS 1.1 만 쓰므로 wolfSSL 을 쓴다(ESP-IDF 의 mbedTLS 3 은 TLS 1.1 이 없다).
 * 클라우드 주소·인증서는 지금은 펌웨어에 고정(나중에 포털 설정으로 옮긴다).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include <fcntl.h>
#include <errno.h>
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "hub_events.h"
#include "lwip/sockets.h"

#include "wolfssl/wolfcrypt/settings.h"
#include "wolfssl/ssl.h"

#include "nvs.h"
#include "hub_store.h"
#include "hub_proxy.h"
#include "hub_rinnai.h"

static const char *TAG = "HUB_PROXY";

extern const uint8_t srv_pem_start[] asm("_binary_srv_pem_start");
extern const uint8_t srv_pem_end[]   asm("_binary_srv_pem_end");

#define UPSTREAM_IP    "3.38.86.244"      /* www.rkiot.net (이름으로 찾으면 허브 DNS 가 자기 자신을 답함) */
#define UPSTREAM_HOST  "www.rkiot.net"
#define MQTT_PORT      8883
#define MAX_LINKS      3                  /* 보일러 + 각방 모듈 + 재접속 여유 1 */
#define IDLE_SEC       200                /* 이만큼 아무것도 안 오가면 죽은 연결로 보고 닫음 */

static WOLFSSL_CTX *s_srv_ctx, *s_cli_ctx;
static volatile int s_links;
static volatile uint8_t s_mode = HUB_MODE_PROXY;
static volatile uint32_t s_mode_gen;          /* 모드가 바뀔 때마다 +1 → 지금 연결들이 알아채고 닫음 */

uint8_t hub_proxy_mode(void) { return s_mode; }

static void mode_load(void)
{
    nvs_handle_t h;
    uint8_t m = HUB_MODE_PROXY;
    if (nvs_open("proxy", NVS_READWRITE, &h) == ESP_OK) {
        hs_get_u8(h, "proxy", "mode", &m);
        nvs_close(h);
    }
    s_mode = m == HUB_MODE_STANDALONE ? HUB_MODE_STANDALONE : HUB_MODE_PROXY;
}

void hub_proxy_render(char *o, size_t cap)
{
    bool sa = s_mode == HUB_MODE_STANDALONE;
    strlcat(o, "<h2>동작 모드</h2><div class=card><form method=post action=/admin/mode id=modef>", cap);
    strlcat(o, sa ? "<label class=chk><input type=radio name=mode value=proxy> <b>프록시 모드</b></label>"
                  : "<label class=chk><input type=radio name=mode value=proxy checked> <b>프록시 모드</b></label>", cap);
    strlcat(o, "<p class=muted style='margin:2px 0 10px 26px'>린나이 클라우드로 중계합니다. 린나이 앱을 그대로 쓰면서 "
               "SmartThings 로도 제어합니다. 인터넷이 필요합니다.</p>", cap);
    strlcat(o, sa ? "<label class=chk><input type=radio name=mode value=standalone checked> <b>단독 모드</b></label>"
                  : "<label class=chk><input type=radio name=mode value=standalone> <b>단독 모드</b></label>", cap);
    strlcat(o, "<p class=muted style='margin:2px 0 10px 26px'>클라우드 없이 허브가 직접 기기를 받습니다. 인터넷이 끊겨도 "
               "SmartThings 로 제어되지만, 린나이 앱으로는 보이지도 제어되지도 않습니다.</p>", cap);
    strlcat(o, "<button class=sv>저장</button></form></div>", cap);
}

bool hub_proxy_apply(const char *form, char *msg, size_t msglen)
{
    char v[16];
    if (httpd_query_key_value(form, "mode", v, sizeof(v)) != ESP_OK ||
        (strcmp(v, "proxy") && strcmp(v, "standalone"))) {
        snprintf(msg, msglen, "잘못된 요청");
        return false;
    }
    uint8_t m = !strcmp(v, "standalone") ? HUB_MODE_STANDALONE : HUB_MODE_PROXY;
    if (m == s_mode) {
        snprintf(msg, msglen, "바뀐 것이 없습니다");
        return true;
    }
    nvs_handle_t h;
    if (nvs_open("proxy", NVS_READWRITE, &h) != ESP_OK) {
        snprintf(msg, msglen, "저장소 열기 실패");
        return false;
    }
    hs_set_u8(h, "mode", m);
    nvs_commit(h);
    nvs_close(h);
    s_mode = m;
    s_mode_gen++;                                    /* 지금 연결 끊기 → 기기가 다시 붙을 때 새 모드 */
    ESP_LOGW(TAG, "동작 모드 → %s (연결 다시 맺음)", m ? "단독" : "프록시");
    snprintf(msg, msglen, "%s로 바꿨습니다. 기기가 다시 접속하면(수십 초) 적용됩니다", m ? "단독 모드" : "프록시 모드");
    return true;
}

/* 집 Wi-Fi IP 가 생길 때까지 최대 wait_s 초 기다린다(부팅 직후 기기가 먼저 붙는 경우). */
static bool sta_ready(int wait_s)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    for (int i = 0; i < wait_s * 10; i++) {
        esp_netif_ip_info_t ip;
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

/* 제한시간 있는 연결. lwIP 의 blocking connect 는 경로가 없으면 ~60초 매달려(실측) 웹서버까지 멈췄다. */
static int tcp_connect(const char *ip, uint16_t port, int timeout_s)
{
    if (!sta_ready(15)) {
        return -1;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    inet_aton(ip, &a.sin_addr);
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int r = connect(s, (struct sockaddr *)&a, sizeof(a));
    if (r < 0 && errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval ct = { .tv_sec = 5 };
        int err = 0;
        socklen_t el = sizeof(err);
        if (select(s + 1, NULL, &w, NULL, &ct) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
            r = 0;
        }
    }
    if (r < 0) {
        close(s);
        return -1;
    }
    fcntl(s, F_SETFL, fl);
    struct timeval tv = { .tv_sec = timeout_s };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return s;
}

static int peer_octet(int fd)
{
    struct sockaddr_storage a;
    socklen_t l = sizeof(a);
    if (getpeername(fd, (struct sockaddr *)&a, &l) < 0) {
        return -1;
    }
    if (a.ss_family == AF_INET) {
        return (int)(ntohl(((struct sockaddr_in *)&a)->sin_addr.s_addr) & 0xff);
    }
#if LWIP_IPV6
    if (a.ss_family == AF_INET6) {               /* 웹서버는 IPv6 겸용 소켓: IPv4-mapped 주소의 마지막 바이트 */
        return ((struct sockaddr_in6 *)&a)->sin6_addr.s6_addr[15];
    }
#endif
    return -1;
}

static const char *mqtt_name(uint8_t b0)
{
    static const char *const n[16] = { "?", "CONNECT", "CONNACK", "PUBLISH", "PUBACK", "?", "?", "?", "SUBSCRIBE",
                                       "SUBACK", "UNSUB", "UNSUBACK", "PINGREQ", "PINGRESP", "DISCONNECT", "?" };
    return n[b0 >> 4];
}

/* 한쪽에서 읽어 다른 쪽으로 쓴다. 0 이하면 연결 종료. 지나가는 바이트는 hub_rinnai 가 엿본다(상태·패킷 경계). */
static int pump(WOLFSSL *from, WOLFSSL *to, const char *arrow, char *buf, int cap, hub_link_tap_t *tap, bool down)
{
    int n = wolfSSL_read(from, buf, cap);
    if (n <= 0) {
        return n;
    }
    if ((buf[0] >> 4) != 12 && (buf[0] >> 4) != 13) {          /* 핑은 로그 생략 */
        ESP_LOGI(TAG, "%s %s %d바이트", arrow, mqtt_name((uint8_t)buf[0]), n);
    }
    if (down) {
        hub_rinnai_tap_down(tap, (const uint8_t *)buf, n);
    } else {
        hub_rinnai_tap_up(tap, (const uint8_t *)buf, n);
    }
    return wolfSSL_write(to, buf, n) == n ? n : -1;
}

/* ---------------------------------------------------------------- 단독 모드: 허브가 MQTT 상대 */

/* 기기 → 허브 패킷 하나에 답한다. 0 = 계속, -1 = 닫기 */
static int broker_reply(WOLFSSL *down, uint8_t b0, const uint8_t *p, int len)
{
    uint8_t r[2 + 2 + 16];
    int n = 0;
    switch (b0 >> 4) {
    case 1:                                         /* CONNECT → CONNACK(수락) */
        ESP_LOGI(TAG, "단독: CONNECT → CONNACK");
        r[n++] = 0x20; r[n++] = 2; r[n++] = 0; r[n++] = 0;
        break;
    case 3: {                                       /* PUBLISH: QoS1 이면 PUBACK, QoS2 면 PUBREC */
        int qos = (b0 >> 1) & 3;
        if (qos && len >= 2) {
            int tl = (p[0] << 8) | p[1];
            if (2 + tl + 2 <= len) {
                r[n++] = qos == 1 ? 0x40 : 0x50; r[n++] = 2; r[n++] = p[2 + tl]; r[n++] = p[3 + tl];
            }
        }
        break;
    }
    case 6:                                         /* PUBREL → PUBCOMP */
        if (len >= 2) {
            r[n++] = 0x70; r[n++] = 2; r[n++] = p[0]; r[n++] = p[1];
        }
        break;
    case 8: {                                       /* SUBSCRIBE → SUBACK(QoS1 까지 허용) */
        if (len < 2) {
            break;
        }
        uint8_t g[16];
        int ng = 0;
        for (int pos = 2; pos + 2 <= len && ng < 16;) {
            int tl = (p[pos] << 8) | p[pos + 1];
            pos += 2 + tl;
            if (pos >= len) {
                break;
            }
            uint8_t q = p[pos++];
            g[ng++] = q > 1 ? 1 : q;
        }
        ESP_LOGI(TAG, "단독: SUBSCRIBE %d개 → SUBACK", ng);
        r[n++] = 0x90; r[n++] = (uint8_t)(2 + ng); r[n++] = p[0]; r[n++] = p[1];
        memcpy(r + n, g, ng);
        n += ng;
        break;
    }
    case 10:                                        /* UNSUBSCRIBE → UNSUBACK */
        if (len >= 2) {
            r[n++] = 0xB0; r[n++] = 2; r[n++] = p[0]; r[n++] = p[1];
        }
        break;
    case 12:                                        /* PINGREQ → PINGRESP */
        r[n++] = 0xD0; r[n++] = 0;
        break;
    case 14:                                        /* DISCONNECT */
        return -1;
    default:                                        /* PUBACK 등(우리 명령에 대한 답): 받기만 */
        break;
    }
    return n && wolfSSL_write(down, r, n) != n ? -1 : 0;
}

/* 단독 모드 연결 유지: 기기 바이트를 엿보고(hub_rinnai) 패킷마다 답하며, SmartThings 명령은 바로 써 넣는다.
 * 돌려주는 값 = closer(1 기기가 닫음, 3 유휴, 4 모드 바뀜, 0 오류) */
static int broker_loop(WOLFSSL *down, int fd, char *buf, hub_link_tap_t *tap, uint32_t gen)
{
    uint8_t *acc = malloc(2048);
    int an = 0, closer = 0;
    int64_t last_io = esp_timer_get_time();
    if (!acc) {
        return 0;
    }
    for (;;) {
        if (s_mode_gen != gen) {
            closer = 4;
            break;
        }
        int cn;
        while ((cn = hub_rinnai_take_cmd(tap, (uint8_t *)buf, 2048)) > 0) {
            if (wolfSSL_write(down, buf, cn) != cn) {
                closer = 1;
                goto out;
            }
            ESP_LOGI(TAG, "SmartThings 명령 → 기기 %d바이트 (단독)", cn);
        }
        if (!wolfSSL_pending(down)) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(fd, &rd);
            struct timeval tick = { .tv_sec = 0, .tv_usec = 300000 };
            int r = select(fd + 1, &rd, NULL, NULL, &tick);
            if (r < 0) {
                break;
            }
            if (r == 0) {
                if (esp_timer_get_time() - last_io > (int64_t)IDLE_SEC * 1000000) {
                    closer = 3;
                    break;
                }
                continue;
            }
        }
        int n = wolfSSL_read(down, acc + an, 2048 - an);
        if (n <= 0) {
            closer = 1;
            break;
        }
        last_io = esp_timer_get_time();
        hub_rinnai_tap_down(tap, acc + an, n);       /* 상태 엿보기(프록시 모드와 같은 길) */
        an += n;
        int off = 0;
        for (;;) {                                   /* 다 모인 패킷마다 답 */
            if (an - off < 2) {
                break;
            }
            int rl = 0, mul = 1, i = 1;
            bool done = false;
            while (i <= 4 && off + i < an) {
                uint8_t b = acc[off + i++];
                rl += (b & 0x7F) * mul;
                mul *= 128;
                if (!(b & 0x80)) {
                    done = true;
                    break;
                }
            }
            if (!done) {
                if (i > 4) {
                    goto out;                        /* 길이 필드가 깨짐 */
                }
                break;                               /* 더 받아야 함 */
            }
            if (off + i + rl > an) {
                break;
            }
            if (broker_reply(down, acc[off], acc + off + i, rl) < 0) {
                closer = 1;
                goto out;
            }
            off += i + rl;
        }
        memmove(acc, acc + off, an - off);
        an -= off;
        if (an >= 2048) {
            ESP_LOGW(TAG, "단독: 너무 큰 패킷 — 닫음");
            break;
        }
    }
out:
    free(acc);
    return closer;
}

/* 클라우드 쪽 연결(TCP+TLS)을 보일러 쪽 TLS 와 '동시에' 진행한다.
 * 순서대로 하면 보일러 TLS(~4초) 뒤 클라우드 TLS(~4.5초)가 이어져, 보일러가 CONNECT 후
 * CONNACK 을 못 받고 끊는다(실측: CONNECT 0.1초 뒤 Wi-Fi 해제). */
typedef struct {
    int fd;
    WOLFSSL *ssl;
    bool ok;
    int dev;                    /* 기기 IP 끝자리(이벤트용) */
    SemaphoreHandle_t done;
} up_job_t;

/* 우선순위는 link_task(5)보다 낮게(4): 각방 모듈은 TLS 가 ~4.5초를 넘으면 포기하므로(실측 4.87초 실패,
 * 4.15초 성공) 기기 쪽 핸드셰이크가 CPU 를 먼저 쓰고, 클라우드 쪽은 그 틈에 진행한다. */
static void up_task(void *arg)
{
    up_job_t *j = arg;
    j->fd = tcp_connect(UPSTREAM_IP, MQTT_PORT, 10);
    j->ssl = j->fd >= 0 ? wolfSSL_new(s_cli_ctx) : NULL;
    if (!j->ssl) {
        ESP_LOGW(TAG, "클라우드 연결 실패 (집 Wi-Fi/인터넷 확인)");
        hub_event(EV_LINK_CLOUD_TLS, NULL, j->dev, -2);
    } else {
        wolfSSL_set_fd(j->ssl, j->fd);
#ifdef HAVE_SNI
        wolfSSL_UseSNI(j->ssl, WOLFSSL_SNI_HOST_NAME, UPSTREAM_HOST, strlen(UPSTREAM_HOST));
#endif
        int64_t t0 = esp_log_timestamp();
        if (wolfSSL_connect(j->ssl) == WOLFSSL_SUCCESS) {
            j->ok = true;
            ESP_LOGI(TAG, "클라우드 쪽 TLS 성공 (%lld ms)", (long long)(esp_log_timestamp() - t0));
            hub_event(EV_LINK_CLOUD_TLS, NULL, j->dev, (int32_t)(esp_log_timestamp() - t0));
        } else {
            ESP_LOGW(TAG, "클라우드 쪽 TLS 실패: err %d", wolfSSL_get_error(j->ssl, -1));
            hub_event(EV_LINK_CLOUD_TLS, NULL, j->dev, wolfSSL_get_error(j->ssl, -1) < 0 ? wolfSSL_get_error(j->ssl, -1) : -1);
        }
    }
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

static void link_task(void *arg)
{
    int down_fd = (int)(intptr_t)arg, up_fd = -1;
    WOLFSSL *down = NULL, *up = NULL;
    char *buf = malloc(2048);
    up_job_t *job = calloc(1, sizeof(*job));
    bool up_waited = false;
    int dev = peer_octet(down_fd), closer = 0;
    int64_t t_up = 0;
    hub_link_tap_t *tap = NULL;
    uint32_t gen = s_mode_gen;
    bool standalone = s_mode == HUB_MODE_STANDALONE;
    if (standalone) {
        free(job);                                  /* 단독: 클라우드 쪽 연결 없음 */
        job = NULL;
    }
    if (job) {
        job->fd = -1;
        job->dev = dev;
        job->done = xSemaphoreCreateBinary();
    }
    if (!standalone && (!job || !job->done || xTaskCreate(up_task, "hub_up", 16384, job, 4, NULL) != pdPASS)) {
        ESP_LOGE(TAG, "클라우드 연결 작업 생성 실패");
        if (job && job->done) {
            vSemaphoreDelete(job->done);
        }
        free(job);
        job = NULL;
        goto done;
    }

    struct timeval tv = { .tv_sec = 30 };
    setsockopt(down_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    down = wolfSSL_new(s_srv_ctx);
    if (!buf || !down) {
        ESP_LOGE(TAG, "메모리 부족으로 세션 생성 실패");
        goto done;
    }
    wolfSSL_set_fd(down, down_fd);
    int64_t t0 = esp_log_timestamp();
    if (wolfSSL_accept(down) != WOLFSSL_SUCCESS) {
        ESP_LOGW(TAG, "보일러 쪽 TLS 실패: err %d", wolfSSL_get_error(down, -1));
        hub_event(EV_LINK_DEV_TLS, NULL, dev, wolfSSL_get_error(down, -1) < 0 ? wolfSSL_get_error(down, -1) : -1);
        goto done;
    }
    ESP_LOGI(TAG, "보일러 쪽 TLS 성공: %s / %s (%lld ms)", wolfSSL_get_version(down), wolfSSL_get_cipher(down),
             (long long)(esp_log_timestamp() - t0));
    hub_event(EV_LINK_DEV_TLS, NULL, dev, (int32_t)(esp_log_timestamp() - t0));

    if (standalone) {
        ESP_LOGI(TAG, "연결됨: 보일러(%s) <-> 허브(단독 모드)", wolfSSL_get_version(down));
        hub_event(EV_LINK_UP, NULL, dev, 1);         /* b=1: 단독 */
        t_up = esp_timer_get_time();
        tv.tv_sec = 0;
        setsockopt(down_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        tap = hub_rinnai_link_open();
        closer = broker_loop(down, down_fd, buf, tap, gen);
        goto out;
    }

    xSemaphoreTake(job->done, portMAX_DELAY);       /* 클라우드 쪽은 이미 같이 진행 중 */
    up_waited = true;
    up_fd = job->fd;
    up = job->ssl;
    job->fd = -1;
    job->ssl = NULL;
    if (!job->ok) {
        goto done;
    }
    ESP_LOGI(TAG, "연결됨: 보일러(%s) <-> 클라우드(%s)", wolfSSL_get_version(down), wolfSSL_get_version(up));
    hub_event(EV_LINK_UP, NULL, dev, 0);
    t_up = esp_timer_get_time();

    tv.tv_sec = 0;                                   /* 중계 중엔 select 로 기다리므로 읽기 제한시간 해제 */
    setsockopt(down_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(up_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tap = hub_rinnai_link_open();
    int64_t last_io = esp_timer_get_time();
    for (;;) {
        if (s_mode_gen != gen) {                     /* 단독 모드로 바뀜 → 끊고 다시 받기 */
            closer = 4;
            break;
        }
        /* SmartThings 명령: 클라우드→기기 패킷 사이에만 끼워 넣는다 */
        int cn;
        while ((cn = hub_rinnai_take_cmd(tap, (uint8_t *)buf, 2048)) > 0) {
            if (wolfSSL_write(down, buf, cn) != cn) {
                closer = 1;
                goto out;
            }
            ESP_LOGI(TAG, "SmartThings 명령 → 기기 %d바이트", cn);
        }
        if (!wolfSSL_pending(down) && !wolfSSL_pending(up)) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(down_fd, &rd);
            FD_SET(up_fd, &rd);
            struct timeval tick = { .tv_sec = 0, .tv_usec = 300000 };   /* 명령 확인 주기 */
            int r = select((down_fd > up_fd ? down_fd : up_fd) + 1, &rd, NULL, NULL, &tick);
            if (r < 0) {
                break;
            }
            if (r == 0) {
                if (esp_timer_get_time() - last_io > (int64_t)IDLE_SEC * 1000000) {
                    ESP_LOGW(TAG, "%d초간 오간 데이터 없음 — 죽은 연결로 보고 닫음", IDLE_SEC);
                    closer = 3;
                    break;
                }
                continue;
            }
            last_io = esp_timer_get_time();
            if (FD_ISSET(down_fd, &rd) && pump(down, up, "보일러→클라우드", buf, 2048, tap, true) <= 0) {
                closer = 1;
                break;
            }
            if (FD_ISSET(up_fd, &rd) && pump(up, down, "클라우드→보일러", buf, 2048, tap, false) <= 0) {
                closer = 2;
                break;
            }
        } else {
            last_io = esp_timer_get_time();
            if (wolfSSL_pending(down) && pump(down, up, "보일러→클라우드", buf, 2048, tap, true) <= 0) {
                closer = 1;
                break;
            }
            if (wolfSSL_pending(up) && pump(up, down, "클라우드→보일러", buf, 2048, tap, false) <= 0) {
                closer = 2;
                break;
            }
        }
    }
out:
    ESP_LOGI(TAG, "연결 종료 (%s)", closer == 1 ? "기기가 닫음" : closer == 2 ? "클라우드가 닫음" : closer == 3 ? "유휴"
                                   : closer == 4 ? "동작 모드 바뀜" : "오류");

done:
    hub_rinnai_link_close(tap);
    if (job) {
        if (!up_waited) {                           /* 보일러 쪽이 먼저 실패: 클라우드 작업 끝날 때까지 */
            xSemaphoreTake(job->done, portMAX_DELAY);
            up = job->ssl;
            up_fd = job->fd;
        }
        vSemaphoreDelete(job->done);
        free(job);
    }
    if (up) {
        wolfSSL_free(up);
    }
    if (down) {
        wolfSSL_free(down);
    }
    if (up_fd >= 0) {
        close(up_fd);
    }
    close(down_fd);
    free(buf);
    {
        uint8_t who[6] = { (uint8_t)closer };
        hub_event(EV_LINK_CLOSE, who, dev, t_up ? (int32_t)((esp_timer_get_time() - t_up) / 1000) : 0);
    }
    s_links--;
    vTaskDelete(NULL);
}

static void listen_task(void *arg)
{
    wolfSSL_Init();
    s_srv_ctx = wolfSSL_CTX_new(wolfSSLv23_server_method());
    s_cli_ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    long len = srv_pem_end - srv_pem_start;
    if (!s_srv_ctx || !s_cli_ctx ||
        wolfSSL_CTX_SetMinVersion(s_srv_ctx, WOLFSSL_TLSV1_1) != WOLFSSL_SUCCESS ||
        wolfSSL_CTX_use_certificate_buffer(s_srv_ctx, srv_pem_start, len, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS ||
        wolfSSL_CTX_use_PrivateKey_buffer(s_srv_ctx, srv_pem_start, len, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
        ESP_LOGE(TAG, "TLS 준비 실패");
        vTaskDelete(NULL);
        return;
    }
    wolfSSL_CTX_set_cipher_list(s_srv_ctx, "ALL");
    wolfSSL_CTX_set_verify(s_cli_ctx, WOLFSSL_VERIFY_NONE, NULL);   /* Pi 판과 동일(클라우드가 사설 CA) */

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(MQTT_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 2) < 0) {
        ESP_LOGE(TAG, "%d 포트 열기 실패", MQTT_PORT);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "기기 대기 :%d (%s 모드, 클라우드 %s)", MQTT_PORT, s_mode ? "단독" : "프록시", UPSTREAM_IP);
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int c = accept(ls, (struct sockaddr *)&from, &fl);
        if (c < 0) {
            continue;
        }
        char ip[16];
        inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
        if (s_links >= MAX_LINKS) {
            ESP_LOGW(TAG, "%s 접속 거절(동시 연결 %d개 초과)", ip, MAX_LINKS);
            hub_event(EV_LINK_REJECT, NULL, (int)(ntohl(from.sin_addr.s_addr) & 0xff), 0);
            close(c);
            continue;
        }
        ESP_LOGI(TAG, "%s 접속", ip);
        hub_event(EV_LINK_OPEN, NULL, (int)(ntohl(from.sin_addr.s_addr) & 0xff), 0);
        s_links++;
        if (xTaskCreate(link_task, "hub_link", 20480, (void *)(intptr_t)c, 5, NULL) != pdPASS) {
            s_links--;
            close(c);
        }
    }
}

void hub_proxy_start(void)
{
    mode_load();
    ESP_LOGI(TAG, "동작 모드: %s", s_mode == HUB_MODE_STANDALONE ? "단독" : "프록시");
    xTaskCreate(listen_task, "hub_proxy", 6144, NULL, 5, NULL);
}

/* 마지막으로 받은 /key/server 답(본문)을 저장/읽기 — 단독 모드나 클라우드가 안 될 때 재사용 */
static void boot_cache_put(const char *body)
{
    nvs_handle_t h;
    size_t n = 2048;
    char *old = malloc(n);
    if (old && nvs_open("proxy", NVS_READWRITE, &h) == ESP_OK) {
        if (hs_get_str(h, "proxy", "boot", old, &n) != ESP_OK || strcmp(old, body)) {   /* 바뀌었을 때만 씀 */
            hs_set_str(h, "boot", body);
            nvs_commit(h);
        }
        nvs_close(h);
    }
    free(old);
}

static bool boot_cache_get(char *out, size_t cap)
{
    nvs_handle_t h;
    size_t n = cap;
    bool ok = false;
    if (nvs_open("proxy", NVS_READWRITE, &h) == ESP_OK) {
        ok = hs_get_str(h, "proxy", "boot", out, &n) == ESP_OK && out[0];
        nvs_close(h);
    }
    return ok;
}

/* 클라우드에 /key/server 를 다시 물어 본문을 buf 에 받는다. 본문 시작 포인터(없으면 NULL) */
static char *boot_fetch(const char *svc, const char *mac, char *buf, int cap)
{
    int s = tcp_connect(UPSTREAM_IP, 80, 8);
    if (s < 0) {
        return NULL;
    }
    int n = snprintf(buf, cap, "GET /key/server HTTP/1.0\r\nHost: " UPSTREAM_HOST "\r\nCk-Service: %s\r\nCk-Mac: %s\r\n\r\n",
                     svc, mac);
    send(s, buf, n, 0);
    int got = 0, r;
    while (got < cap - 1 && (r = recv(s, buf + got, cap - 1 - got, 0)) > 0) {
        got += r;
    }
    close(s);
    buf[got] = '\0';
    char *body = strstr(buf, "\r\n\r\n");
    return body ? body + 4 : NULL;
}

/* 보일러의 GET /key/server: 프록시 모드면 클라우드에 다시 물어 답하고(그 답을 저장), 클라우드가 안 되거나
 * 단독 모드면 저장해 둔 마지막 답을 준다(기기에 알려 줄 접속 주소는 같은 rkiot.net → 허브 DNS 가 허브로). */
esp_err_t hub_proxy_bootstrap_handler(httpd_req_t *req)
{
    char svc[32] = "", mac[32] = "";
    httpd_req_get_hdr_value_str(req, "Ck-Service", svc, sizeof(svc));
    httpd_req_get_hdr_value_str(req, "Ck-Mac", mac, sizeof(mac));
    ESP_LOGI(TAG, "부트스트랩 요청 (Ck-Mac %s, %s 모드)", mac, s_mode == HUB_MODE_STANDALONE ? "단독" : "프록시");

    char *buf = malloc(2048);
    char *body = NULL;
    if (buf && s_mode == HUB_MODE_PROXY) {
        body = boot_fetch(svc, mac, buf, 2048);
        if (body && body[0]) {
            boot_cache_put(body);
        } else {
            body = NULL;
            ESP_LOGW(TAG, "부트스트랩: 클라우드 응답 없음 — 저장해 둔 답 사용");
        }
    }
    if (buf && !body && boot_cache_get(buf, 2048)) {
        body = buf;
    }
    if (!body) {
        free(buf);
        ESP_LOGW(TAG, "부트스트랩: 줄 답이 없음(클라우드 실패 + 저장된 답 없음)");
        hub_event(EV_BOOTSTRAP, NULL, peer_octet(httpd_req_to_sockfd(req)), 0);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upstream");
    }
    ESP_LOGI(TAG, "부트스트랩 응답 (%d바이트)", (int)strlen(body));
    hub_event(EV_BOOTSTRAP, NULL, peer_octet(httpd_req_to_sockfd(req)), 1);
    /* Pi fakesrv 처럼 헤더+본문을 '한 번에' 보낸다. httpd_resp_send 는 헤더와 본문을 따로 보내서
     * (TCP 조각 2개) GainSpan 이 첫 조각만 읽고 실패 처리하는 것으로 의심됨. */
    int bl = (int)strlen(body);
    char *out = malloc(bl + 160);
    esp_err_t e = ESP_FAIL;
    if (out) {
        int ol = snprintf(out, bl + 160,
                          "HTTP/1.1 200 OK\r\nContent-Type: application/xml; charset=utf-8\r\n"
                          "Content-Length: %d\r\n\r\n%s", bl, body);
        e = httpd_send(req, out, ol) == ol ? ESP_OK : ESP_FAIL;
        free(out);
    }
    free(buf);
    /* 다 보낸 뒤 연결을 닫아 응답 끝을 알린다(Pi fakesrv 와 동일). */
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return e;
}
