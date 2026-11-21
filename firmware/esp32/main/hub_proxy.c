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
#include "wolfssl/wolfcrypt/error-crypt.h"
#include <time.h>
#include <stdatomic.h>

#include "nvs.h"
#include "hub_store.h"
#include "esp_random.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "hub_proxy.h"
#include "hub_rinnai.h"
#include "hub_wifi.h"

static const char *TAG = "HUB_PROXY";

/* 린나이 클라우드의 사설 CA "ckstack"(Ckstack Inc, 2024-01-24 ~ 2123-12-31, SHA-256 C7:0B:E6:E2:…:1E:57).
 * 서버 인증서(CN=dev2.ckpush.com)가 이 CA 로 서명돼 있어, 이걸로 허브 → 클라우드 구간의 가짜 서버를 막는다.
 * 이름(CN)이 접속 이름 www.rkiot.net 과 달라, 체인과 함께 그 실제 이름(CLOUD_CN)을 검사한다. */
#define CLOUD_CN "dev2.ckpush.com"           /* 클라우드 인증서의 실제 이름(SAN 없음, CN 만). 접속 이름 www.rkiot.net 과 다르다 */
extern const uint8_t cloud_ca_start[] asm("_binary_cloud_ca_crt_start");
extern const uint8_t cloud_ca_end[]   asm("_binary_cloud_ca_crt_end");

/* 린나이 클라우드. 이름·포트는 기기 펌웨어가 정한 값이라 고정, IP 만 집 Wi-Fi 쪽 DNS 로 조회(resolve_up) */
#define DEF_UP_IP      "3.38.86.244"      /* 처음 쓰는 값. 이후엔 집 Wi-Fi 쪽 DNS 로 서버 이름을 조회해 갱신(resolve_up) */
#define DNS_REFRESH_US (10LL * 60 * 1000000) /* 조회 결과 재사용 시간 */
#define UP_HOST        "www.rkiot.net"
#define MQTT_PORT      8883               /* 기기 → 허브, 허브 → 클라우드 모두 같은 포트(기기는 허브를 클라우드로 안다) */
#define BOOT_PORT      80                 /* /key/server 도 마찬가지 */
#define PEM_MAX        6144               /* 인증서+키 PEM 최대(RSA-2048 은 약 3KB) */
#define MAX_LINKS      3                  /* 보일러 + 각방 모듈 + 재접속 여유 1 */
#define IDLE_SEC       200                /* 이만큼 아무것도 안 오가면 죽은 연결로 보고 닫음 */

static WOLFSSL_CTX *s_srv_ctx, *s_cli_ctx;
static SemaphoreHandle_t s_ctx_mx;            /* s_srv_ctx 교체(인증서 바꿈)와 wolfSSL_new 사이 */
static struct {
    char host[64];
    char ip[16];
} s_up = { UP_HOST, DEF_UP_IP };
static size_t s_pem_len;                      /* 0 = 펌웨어 내장 인증서 사용 */
static SemaphoreHandle_t s_dns_mx;
static int64_t s_dns_at;                      /* 마지막 조회 성공 시각(0 = 아직) */
static char s_dns_from[16];                   /* 그때 물어본 DNS 서버 */
static atomic_int s_links;

/* TLS 핸드셰이크는 한 번에 한 연결씩. 연결 하나가 핸드셰이크하는 동안 스택만 37KB(link 20KB + up 16KB)에 wolfSSL
 * 세션 둘이 더 든다. 부팅 직후 보일러·각방 모듈이 1~2초 간격으로 같이 붙으면 힙이 바닥나(최저 400~900바이트 실측)
 * 한쪽 TLS 가 -125(메모리)로 실패하고, 그 순간 다른 할당(Wi-Fi·웹·Zigbee)도 위험해진다.
 * 뒤에 온 연결은 앞 연결이 끝나길 최대 HS_WAIT_MS 만 기다린다 — 기기는 TLS 가 4.5초쯤 넘으면 포기하므로(실측)
 * 더 오래 붙잡지 않고, 그때는 예전처럼 같이 진행한다. */
#define HS_WAIT_MS 2500
/* 태스크 스택. 예전 20KB/16KB 는 실측(/api/v1/state 의 stk_link_free·stk_up_free)으로 2.2KB/1.7KB 만 쓰였다
 * (wolfSSL 이 큰 수 연산을 힙에서 한다). 3~4배 여유로 줄여 연결당 22KB 를 돌려받는다. */
#define LINK_STACK 8192
#define UP_STACK   6144
static SemaphoreHandle_t s_hs_mx;
static atomic_int s_hs_busy;                  /* 핸드셰이크 중인 연결 수(가스 요금 조회가 이때를 피한다) */
static atomic_int s_stk_link, s_stk_up;       /* 태스크 스택이 가장 적게 남았던 바이트(0 = 아직 모름) — 스택 크기 조정용 */

static void note_stack(atomic_int *slot)
{
    int v = (int)uxTaskGetStackHighWaterMark(NULL), cur = atomic_load(slot);
    if (cur == 0 || v < cur) {
        atomic_store(slot, v);
    }
}

static void hs_end(bool *open, bool *held)
{
    if (!*open) {
        return;
    }
    *open = false;
    atomic_fetch_sub(&s_hs_busy, 1);
    if (*held) {
        *held = false;
        xSemaphoreGive(s_hs_mx);
    }
}

bool hub_proxy_handshaking(void) { return atomic_load(&s_hs_busy) > 0; }

void hub_proxy_stack_free(int *link, int *up)
{
    if (link) *link = atomic_load(&s_stk_link);
    if (up) *up = atomic_load(&s_stk_up);
}
#define RELAY_READ_SEC 10                    /* 중계 중 한 번 읽기의 최대 대기(레코드 일부만 오고 멈추면 끊음) */
static volatile uint8_t s_mode = HUB_MODE_PROXY;
static volatile uint32_t s_mode_gen;          /* 모드가 바뀔 때마다 +1 → 지금 연결들이 알아채고 닫음 */

uint8_t hub_proxy_mode(void) { return s_mode; }

/* ---------------------------------------------------------------- 클라우드 주소 조회
 * 허브 자신의 리졸버를 쓰지 않는다: lwIP 의 DNS 서버는 전역이고 허브 AP 가 자기 자신을 DNS 로 내놓아서,
 * 이름으로 찾으면 허브 DNS(rkiot.net → 허브)가 답할 수 있다. 그래서 집 Wi-Fi(STA) 쪽 DNS 에 직접 UDP 로 묻는다. */

/* server(네트워크 바이트 순서)에 host 의 A 레코드를 묻는다. 성공하면 out 에 "a.b.c.d" */
static bool dns_query_a(uint32_t server, const char *host, char out[16])
{
    uint8_t q[300], r[512];
    uint16_t id = (uint16_t)esp_random();
    int n = 0;
    q[n++] = id >> 8; q[n++] = id & 0xff;
    q[n++] = 0x01; q[n++] = 0x00;                    /* 재귀 요청 */
    q[n++] = 0; q[n++] = 1;                          /* 질문 1개 */
    memset(q + n, 0, 6);
    n += 6;
    for (const char *p = host; *p;) {                /* www.rkiot.net → 3www5rkiot3net0 */
        const char *dot = strchr(p, '.');
        int l = dot ? (int)(dot - p) : (int)strlen(p);
        if (l <= 0 || l > 63 || n + l + 6 > (int)sizeof(q)) {
            return false;
        }
        q[n++] = (uint8_t)l;
        memcpy(q + n, p, l);
        n += l;
        p += l + (dot ? 1 : 0);
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;                          /* A */
    q[n++] = 0; q[n++] = 1;                          /* IN */

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = server };
    int got = -1;
    if (sendto(s, q, n, 0, (struct sockaddr *)&a, sizeof(a)) == n) {
        got = recv(s, r, sizeof(r), 0);
    }
    close(s);
    if (got < 12 || ((r[0] << 8) | r[1]) != id || (r[3] & 0x0F) != 0) {
        return false;                                /* 응답 없음 / 다른 응답 / 오류 코드 */
    }
    int an = (r[6] << 8) | r[7];
    int pos = n;                                     /* 질문 부분은 우리가 보낸 것과 같다 */
    for (int i = 0; i < an && pos < got; i++) {
        if ((r[pos] & 0xC0) == 0xC0) {               /* 이름: 압축 포인터 */
            pos += 2;
        } else {
            while (pos < got && r[pos]) {
                pos += r[pos] + 1;
            }
            pos++;
        }
        if (pos + 10 > got) {
            break;
        }
        int type = (r[pos] << 8) | r[pos + 1], rdlen = (r[pos + 8] << 8) | r[pos + 9];
        pos += 10;
        if (type == 1 && rdlen == 4 && pos + 4 <= got) {
            snprintf(out, 16, "%u.%u.%u.%u", r[pos], r[pos + 1], r[pos + 2], r[pos + 3]);
            return true;
        }
        pos += rdlen;                                /* CNAME 등은 건너뜀 */
    }
    return false;
}

/* 클라우드 서버 이름을 집 Wi-Fi 쪽 DNS(DHCP 로 받은 DNS → 보조 → 공유기)로 조회해 s_up.ip 를 갱신.
 * 10분 안에 조회했으면 그대로. 실패하면 마지막으로 알던 IP(저장값)를 계속 쓴다. */
static void resolve_up(bool force)
{
    if (!s_dns_mx) {
        return;
    }
    xSemaphoreTake(s_dns_mx, portMAX_DELAY);
    if (!force && s_dns_at && esp_timer_get_time() - s_dns_at < DNS_REFRESH_US) {
        xSemaphoreGive(s_dns_mx);
        return;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    uint32_t ap = hub_wifi_ap_ip(), cand[3] = { 0 };
    int nc = 0;
    esp_netif_dns_info_t d;
    for (int k = 0; sta && k < 2; k++) {
        if (esp_netif_get_dns_info(sta, k == 0 ? ESP_NETIF_DNS_MAIN : ESP_NETIF_DNS_BACKUP, &d) == ESP_OK &&
            d.ip.type == ESP_IPADDR_TYPE_V4 && d.ip.u_addr.ip4.addr && d.ip.u_addr.ip4.addr != ap) {
            cand[nc++] = d.ip.u_addr.ip4.addr;       /* 허브 자기 AP 주소면 빼고 */
        }
    }
    esp_netif_ip_info_t ip;
    if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.gw.addr) {
        cand[nc++] = ip.gw.addr;                     /* 공유기(대개 DNS 중계도 함) */
    }
    char got[16];
    bool ok = false;
    for (int k = 0; k < nc && !ok; k++) {
        ok = dns_query_a(cand[k], s_up.host, got);
        if (ok) {
            struct in_addr ia = { .s_addr = cand[k] };
            inet_ntoa_r(ia, s_dns_from, sizeof(s_dns_from));
        }
    }
    if (ok) {
        s_dns_at = esp_timer_get_time();
        ESP_LOGI(TAG, "클라우드 주소 조회: %s = %s (집 DNS %s)", s_up.host, got, s_dns_from);
        if (strcmp(got, s_up.ip)) {
            ESP_LOGW(TAG, "클라우드 %s 주소: %s → %s (DNS %s)", s_up.host, s_up.ip, got, s_dns_from);
            strlcpy(s_up.ip, got, sizeof(s_up.ip));
            nvs_handle_t h;
            if (nvs_open("proxy", NVS_READWRITE, &h) == ESP_OK) {   /* 다음 부팅에 DNS 가 안 돼도 쓰게 */
                hs_set_str(h, "up_ip", got);
                nvs_commit(h);
                nvs_close(h);
            }
        }
    } else {
        ESP_LOGW(TAG, "클라우드 %s 조회 실패(DNS 후보 %d개) — 마지막 주소 %s 사용", s_up.host, nc, s_up.ip);
    }
    xSemaphoreGive(s_dns_mx);
}

static void mode_load(void)
{
    nvs_handle_t h;
    uint8_t m = HUB_MODE_PROXY;
    if (nvs_open("proxy", NVS_READWRITE, &h) == ESP_OK) {
        hs_get_u8(h, "proxy", "mode", &m);
        hs_erase(h, "boot");                         /* 예전 공용 부트스트랩 캐시(이제 기기별 bt########) */
        size_t n = sizeof(s_up.ip);
        if (hs_get_str(h, "proxy", "up_ip", s_up.ip, &n) != ESP_OK || !s_up.ip[0]) {
            strlcpy(s_up.ip, DEF_UP_IP, sizeof(s_up.ip));
        }
        nvs_close(h);
    }
    s_mode = m == HUB_MODE_STANDALONE ? HUB_MODE_STANDALONE : HUB_MODE_PROXY;
}

/* 기기 쪽 TLS 서버 컨텍스트: 인증서+개인키 PEM 하나로. 실패하면 NULL */
static WOLFSSL_CTX *make_srv_ctx(const uint8_t *pem, long len)
{
    WOLFSSL_CTX *c = wolfSSL_CTX_new(wolfSSLv23_server_method());
    if (!c) {
        return NULL;
    }
    if (wolfSSL_CTX_SetMinVersion(c, WOLFSSL_TLSV1_1) != WOLFSSL_SUCCESS ||   /* 보일러 모듈은 TLS 1.1 */
        wolfSSL_CTX_use_certificate_buffer(c, pem, len, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS ||
        wolfSSL_CTX_use_PrivateKey_buffer(c, pem, len, WOLFSSL_FILETYPE_PEM) != WOLFSSL_SUCCESS) {
        wolfSSL_CTX_free(c);
        return NULL;
    }
    wolfSSL_CTX_set_cipher_list(c, "ALL");
    return c;
}

/* 허브가 만들어 저장해 둔 인증서. 없거나 못 읽으면 NULL(→ listen_task 가 먼저 만든다) */
static WOLFSSL_CTX *load_srv_ctx(void)
{
    nvs_handle_t h;
    WOLFSSL_CTX *c = NULL;
    uint8_t *pem = malloc(PEM_MAX);
    size_t n = PEM_MAX;
    if (pem && nvs_open("proxy", NVS_READWRITE, &h) == ESP_OK) {
        if (hs_get_blob(h, "proxy", "pem", pem, &n) == ESP_OK && n > 0) {
            c = make_srv_ctx(pem, (long)n);
            if (c) {
                s_pem_len = n;
                ESP_LOGI(TAG, "기기용 인증서: 허브 자체 인증서 (%u바이트)", (unsigned)n);
            } else {
                ESP_LOGW(TAG, "저장된 인증서를 읽지 못함 — 새로 만듦");
            }
        }
        nvs_close(h);
    }
    free(pem);
    return c;
}

/* ---------------------------------------------------------------- 허브 자체 인증서
 * 기기는 서버 인증서를 검증하지 않는다(핀닝 없음, MITM 실험으로 확인) → 아무 자체 서명 인증서면 된다.
 * 펌웨어에 인증서·개인키를 넣지 않고, 허브가 처음(또는 초기화 뒤) 한 번 RSA-2048 키와 인증서를 만들어 저장한다.
 * 보일러 모듈이 TLS 1.1 이라 RSA 로(ECDSA 지원 불확실). 실측 3초 — listen_task 가 기기를 받기 전에 끝날 때까지 기다린다.
 * arg = 끝나면 알릴 세마포어. 큰 스택(8KB)이 잠깐만 필요해서 따로 태스크로 돌리고 끝나면 지운다. */
static void certgen_task(void *arg)
{
    int64_t t0 = esp_timer_get_time();
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&rng);
    char *pem = malloc(PEM_MAX);
    int r = pem ? 0 : -1;
    const char *pers = "lynnai-hub-cert";
    if (!r) r = mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &ent, (const uint8_t *)pers, strlen(pers));
    if (!r) r = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (!r) r = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random, &rng, 2048, 65537);
    if (!r) {
        uint8_t serial[16];
        esp_fill_random(serial, sizeof(serial));
        serial[0] &= 0x7F;                           /* 양수 */
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        r = mbedtls_x509write_crt_set_subject_name(&crt, "C=KR,O=rkiot,CN=rkiot.net");
        if (!r) r = mbedtls_x509write_crt_set_issuer_name(&crt, "C=KR,O=rkiot,CN=rkiot.net");
        if (!r) r = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
        if (!r) r = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20491231235959");
        if (!r) {
            mbedtls_x509_san_list n3 = { .node = { .type = MBEDTLS_X509_SAN_DNS_NAME,
                                                   .san.unstructured_name = { .p = (uint8_t *)"www.rkiot.net", .len = 13 } } };
            mbedtls_x509_san_list n2 = { .node = { .type = MBEDTLS_X509_SAN_DNS_NAME,
                                                   .san.unstructured_name = { .p = (uint8_t *)"*.rkiot.net", .len = 11 } },
                                         .next = &n3 };
            mbedtls_x509_san_list n1 = { .node = { .type = MBEDTLS_X509_SAN_DNS_NAME,
                                                   .san.unstructured_name = { .p = (uint8_t *)"rkiot.net", .len = 9 } },
                                         .next = &n2 };
            r = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &n1);
        }
        if (!r) r = mbedtls_x509write_crt_pem(&crt, (uint8_t *)pem, PEM_MAX, mbedtls_ctr_drbg_random, &rng);
        if (!r) {
            size_t cl = strlen(pem);
            r = mbedtls_pk_write_key_pem(&key, (uint8_t *)pem + cl, PEM_MAX - cl);
        }
    }
    WOLFSSL_CTX *c = NULL;
    size_t pl = 0;
    if (!r) {
        pl = strlen(pem);
        c = make_srv_ctx((const uint8_t *)pem, (long)pl);   /* 만든 것을 실제로 읽혀 본 뒤에만 저장·교체 */
    }
    if (c) {
        nvs_handle_t h;
        if (nvs_open("proxy", NVS_READWRITE, &h) == ESP_OK) {
            hs_set_blob(h, "pem", pem, pl);
            nvs_commit(h);
            nvs_close(h);
        }
        xSemaphoreTake(s_ctx_mx, portMAX_DELAY);
        WOLFSSL_CTX *old = s_srv_ctx;
        s_srv_ctx = c;                               /* 새 연결부터(연결 중인 세션은 옛 것을 잡고 있다가 끝남) */
        s_pem_len = pl;
        xSemaphoreGive(s_ctx_mx);
        if (old) {
            wolfSSL_CTX_free(old);
        }
        ESP_LOGW(TAG, "허브 자체 인증서 만듦 (RSA-2048, %u바이트, %lld초)", (unsigned)pl,
                 (long long)((esp_timer_get_time() - t0) / 1000000));
    } else {
        ESP_LOGE(TAG, "자체 인증서 만들기 실패(mbedtls -0x%04x)", (unsigned)-r);
    }
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&rng);
    mbedtls_entropy_free(&ent);
    free(pem);
    xSemaphoreGive((SemaphoreHandle_t)arg);
    vTaskDelete(NULL);
}

static WOLFSSL *new_srv_ssl(void)
{
    xSemaphoreTake(s_ctx_mx, portMAX_DELAY);
    WOLFSSL *ssl = s_srv_ctx ? wolfSSL_new(s_srv_ctx) : NULL;
    xSemaphoreGive(s_ctx_mx);
    return ssl;
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
    esp_err_t e = hs_set_u8(h, "mode", m);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e != ESP_OK) {                               /* 저장이 안 되면 바꾸지 않는다(재부팅 후 되돌아가지 않게) */
        ESP_LOGE(TAG, "동작 모드 저장 실패: %s", esp_err_to_name(e));
        snprintf(msg, msglen, "저장하지 못했습니다(%s). 모드는 그대로입니다", esp_err_to_name(e));
        return false;
    }
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

/* 상대 주소(IPv4, 네트워크 순서). IPv4-mapped IPv6 도 */
static uint32_t peer_ip(int fd)
{
    struct sockaddr_storage a;
    socklen_t l = sizeof(a);
    if (getpeername(fd, (struct sockaddr *)&a, &l) < 0) {
        return 0;
    }
    if (a.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&a)->sin_addr.s_addr;
    }
#if LWIP_IPV6
    if (a.ss_family == AF_INET6) {              /* ::ffff:a.b.c.d (IPv4-mapped) 만 IPv4 로 본다 — 진짜 IPv6 는 0(모름) */
        static const uint8_t MAPPED[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        const uint8_t *b = ((struct sockaddr_in6 *)&a)->sin6_addr.s6_addr;
        uint32_t v;
        if (memcmp(b, MAPPED, 12)) {
            return 0;
        }
        memcpy(&v, b + 12, 4);
        return v;
    }
#endif
    return 0;
}

void hub_proxy_relink(void)
{
    s_mode_gen++;                                    /* 연결 태스크들이 알아채고 닫는다 → 기기가 수 초 안에 다시 붙음 */
}

static const char *mqtt_name(uint8_t b0)
{
    static const char *const n[16] = { "?", "CONNECT", "CONNACK", "PUBLISH", "PUBACK", "?", "?", "?", "SUBSCRIBE",
                                       "SUBACK", "UNSUB", "UNSUBACK", "PINGREQ", "PINGRESP", "DISCONNECT", "?" };
    return n[b0 >> 4];
}

/* 한쪽에서 읽어 다른 쪽으로 쓴다. 음수면 연결 종료, 0 = 아직 넘길 데이터 없음(끊지 않는다).
 * 0 인 경우: 앱 데이터가 없는 TLS 기록(세션 티켓·키 갱신)만 왔거나, 기록이 반만 오고 RELAY_READ_SEC 가 지남 —
 * 읽은 조각은 wolfSSL 이 갖고 있다가 나머지가 오면 잇는다. 지나가는 바이트는 hub_rinnai 가 엿본다(상태·패킷 경계). */
static int pump(WOLFSSL *from, WOLFSSL *to, const char *arrow, char *buf, int cap, hub_link_tap_t *tap, bool down)
{
    int n = wolfSSL_read(from, buf, cap);
    if (n <= 0) {
        return wolfSSL_get_error(from, n) == WOLFSSL_ERROR_WANT_READ ? 0 : -1;
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
        if (len >= 2) {                                 /* 기기가 올린 것(토픽 끝: inf/res/stg) — 프록시 로그와 같은 모양 */
            int tl0 = (p[0] << 8) | p[1];
            const uint8_t *tp = p + 2, *sl = NULL;
            for (int i = 0; i < tl0 && 2 + i < len; i++) {
                if (tp[i] == '/') {
                    sl = tp + i;
                }
            }
            int sn = sl ? (int)(tp + tl0 - sl) : 0;
            ESP_LOGI(TAG, "단독: 기기→허브 PUBLISH %.*s %d바이트", sn > 0 && sn < 8 ? sn : 0, sl ? (const char *)sl : "", len);
        }
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
            ESP_LOGI(TAG, "기기로 보냄 %d바이트 (단독: 상태 요청·시간·SmartThings 명령)", cn);
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
            if (wolfSSL_get_error(down, n) == WOLFSSL_ERROR_WANT_READ) {
                continue;                            /* 기록이 반만 왔거나 앱 데이터 없는 기록 — 끊지 않고 다음 바퀴(유휴 감시는 그대로) */
            }
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
    if (sta_ready(15)) {
        resolve_up(false);                           /* 10분 지났으면 집 DNS 로 다시 조회 */
    }
    j->fd = tcp_connect(s_up.ip, MQTT_PORT, 10);
    j->ssl = j->fd >= 0 ? wolfSSL_new(s_cli_ctx) : NULL;
    if (!j->ssl) {
        ESP_LOGW(TAG, "클라우드 연결 실패 (집 Wi-Fi/인터넷 확인)");
        hub_event(EV_LINK_CLOUD_TLS, NULL, j->dev, -2);
    } else {
        wolfSSL_set_fd(j->ssl, j->fd);
        wolfSSL_check_domain_name(j->ssl, CLOUD_CN);   /* 체인(사설 CA)에 더해 서버 이름까지 고정 */
#ifdef HAVE_SNI
        wolfSSL_UseSNI(j->ssl, WOLFSSL_SNI_HOST_NAME, s_up.host, strlen(s_up.host));
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
    note_stack(&s_stk_up);
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
    /* 핸드셰이크 차례를 기다린다(위 HS_WAIT_MS 설명). 못 잡아도 진행한다 */
    bool hs_held = s_hs_mx && xSemaphoreTake(s_hs_mx, pdMS_TO_TICKS(HS_WAIT_MS)) == pdTRUE;
    bool hs_open = true;
    atomic_fetch_add(&s_hs_busy, 1);
    if (standalone) {
        free(job);                                  /* 단독: 클라우드 쪽 연결 없음 */
        job = NULL;
    }
    if (job) {
        job->fd = -1;
        job->dev = dev;
        job->done = xSemaphoreCreateBinary();
    }
    if (!standalone && (!job || !job->done || xTaskCreate(up_task, "hub_up", UP_STACK, job, 4, NULL) != pdPASS)) {
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
    down = new_srv_ssl();
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
        tv.tv_sec = RELAY_READ_SEC;
        setsockopt(down_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        note_stack(&s_stk_link);
        hs_end(&hs_open, &hs_held);
        tap = hub_rinnai_link_open(peer_ip(down_fd));
        closer = broker_loop(down, down_fd, buf, tap, gen);
        goto out;
    }

    xSemaphoreTake(job->done, portMAX_DELAY);       /* 클라우드 쪽은 이미 같이 진행 중 */
    up_waited = true;
    note_stack(&s_stk_link);
    hs_end(&hs_open, &hs_held);                     /* 양쪽 핸드셰이크 끝 — 다음 연결 차례 */
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

    tv.tv_sec = RELAY_READ_SEC;                      /* select 뒤 읽기: TLS 레코드를 반만 보내고 멈추는 상대에 매달리지 않게 */
    setsockopt(down_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(up_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tap = hub_rinnai_link_open(peer_ip(down_fd));
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
            ESP_LOGI(TAG, "기기로 보냄 %d바이트 (상태 요청·SmartThings 명령)", cn);
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
            if (FD_ISSET(down_fd, &rd) && pump(down, up, "보일러→클라우드", buf, 2048, tap, true) < 0) {
                closer = 1;
                break;
            }
            if (FD_ISSET(up_fd, &rd) && pump(up, down, "클라우드→보일러", buf, 2048, tap, false) < 0) {
                closer = 2;
                break;
            }
        } else {
            last_io = esp_timer_get_time();
            if (wolfSSL_pending(down) && pump(down, up, "보일러→클라우드", buf, 2048, tap, true) < 0) {
                closer = 1;
                break;
            }
            if (wolfSSL_pending(up) && pump(up, down, "클라우드→보일러", buf, 2048, tap, false) < 0) {
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
    note_stack(&s_stk_link);
    hs_end(&hs_open, &hs_held);                     /* 실패로 빠져나온 경우(클라우드 작업까지 끝난 뒤) */
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
    atomic_fetch_sub(&s_links, 1);
    vTaskDelete(NULL);
}

/* 클라우드 인증서 검증 콜백: 서명·체인 실패는 그대로 거절. 날짜 오류는 허브 시계가 아직 안 맞았을 때(SNTP 전,
 * 2024년 이전으로 보일 때)만 넘긴다 — 부팅 직후 첫 연결이 시계 때문에 실패하지 않게. */
static int cloud_verify_cb(int ok, WOLFSSL_X509_STORE_CTX *st)
{
    if (ok) {
        return 1;
    }
    int err = st ? st->error : 0;
    if ((err == ASN_BEFORE_DATE_E || err == ASN_AFTER_DATE_E) && time(NULL) < 1704067200) {   /* 2024-01-01 */
        return 1;
    }
    ESP_LOGW(TAG, "클라우드 인증서 검증 실패(%d) — 연결 안 함", err);
    return 0;
}

static void listen_task(void *arg)
{
    wolfSSL_Init();
    s_srv_ctx = load_srv_ctx();
    while (!s_srv_ctx) {                             /* 첫 부팅·초기화 뒤: 기기를 받기 전에 인증서부터 */
        ESP_LOGW(TAG, "기기용 인증서 없음 — 지금 만든다(몇 초, 그동안 기기 접속은 실패 후 재시도)");
        SemaphoreHandle_t done = xSemaphoreCreateBinary();
        if (done && xTaskCreate(certgen_task, "certgen", 8192, done, 4, NULL) == pdPASS) {
            xSemaphoreTake(done, portMAX_DELAY);
        }
        if (done) {
            vSemaphoreDelete(done);
        }
        if (!s_srv_ctx) {
            vTaskDelay(pdMS_TO_TICKS(30000));        /* 실패(메모리 부족 등): 잠시 뒤 다시 */
        }
    }
    s_cli_ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (s_cli_ctx && wolfSSL_CTX_SetMinVersion(s_cli_ctx, WOLFSSL_TLSV1_2) != WOLFSSL_SUCCESS) {   /* 클라우드는 TLS 1.3 */
        ESP_LOGW(TAG, "클라우드 쪽 TLS 최소 버전(1.2) 설정 실패");
    }
    if (!s_srv_ctx || !s_cli_ctx) {
        ESP_LOGE(TAG, "TLS 준비 실패");
        vTaskDelete(NULL);
        return;
    }
    /* 부팅 직후엔 시계가 1970년이라 CA 를 '아직 유효 전'으로 거절한다(-150) → 읽을 때는 날짜 오류 허용.
     * 접속할 때의 날짜는 cloud_verify_cb 가 시계 상태를 보고 판단. 길이 -1 = EMBED_TXTFILES 가 붙인 끝 NUL 제외 */
    int car = wolfSSL_CTX_load_verify_buffer_ex(s_cli_ctx, cloud_ca_start, cloud_ca_end - cloud_ca_start - 1,
                                                WOLFSSL_FILETYPE_PEM, 0, WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY);
    if (car == WOLFSSL_SUCCESS) {
        ESP_LOGI(TAG, "클라우드 CA 고정(ckstack) — 클라우드 인증서를 검증함");
        wolfSSL_CTX_set_verify(s_cli_ctx, WOLFSSL_VERIFY_PEER, cloud_verify_cb);   /* 사설 CA 로 고정 */
    } else {
        /* 검증을 끄고 붙지 않는다: 검증 대상(CA)이 없으면 모든 클라우드 연결이 실패하게 둔다(프록시 중단, 단독은 영향 없음) */
        ESP_LOGE(TAG, "클라우드 CA 읽기 실패(%d) — 클라우드에 붙지 않음(펌웨어 확인 필요)", car);
        wolfSSL_CTX_set_verify(s_cli_ctx, WOLFSSL_VERIFY_PEER, NULL);
    }

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(MQTT_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 2) < 0) {
        ESP_LOGE(TAG, "%d 포트 열기 실패", MQTT_PORT);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "기기 대기 :%d (%s 모드, 클라우드 %s = %s)", MQTT_PORT, s_mode ? "단독" : "프록시", s_up.host, s_up.ip);
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int c = accept(ls, (struct sockaddr *)&from, &fl);
        if (c < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));          /* 소켓 고갈 등 — 헛돌지 않게 */
            continue;
        }
        char ip[16];
        inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
        if (!hub_wifi_is_ap_peer(from.sin_addr.s_addr)) {   /* 기기는 허브 AP 에만 있다 — 집 LAN 쪽 접속은 받지 않음 */
            ESP_LOGW(TAG, "%s 접속 거절(허브 AP 대역 아님)", ip);
            close(c);
            continue;
        }
        if (atomic_load(&s_links) >= MAX_LINKS) {
            ESP_LOGW(TAG, "%s 접속 거절(동시 연결 %d개 초과)", ip, MAX_LINKS);
            hub_event(EV_LINK_REJECT, NULL, (int)(ntohl(from.sin_addr.s_addr) & 0xff), 0);
            close(c);
            continue;
        }
        ESP_LOGI(TAG, "%s 접속", ip);
        hub_event(EV_LINK_OPEN, NULL, (int)(ntohl(from.sin_addr.s_addr) & 0xff), 0);
        atomic_fetch_add(&s_links, 1);
        if (xTaskCreate(link_task, "hub_link", LINK_STACK, (void *)(intptr_t)c, 5, NULL) != pdPASS) {
            atomic_fetch_sub(&s_links, 1);
            close(c);
        }
    }
}

void hub_proxy_start(void)
{
    s_ctx_mx = xSemaphoreCreateMutex();
    s_hs_mx = xSemaphoreCreateMutex();
    s_dns_mx = xSemaphoreCreateMutex();
    mode_load();
    ESP_LOGI(TAG, "동작 모드: %s", s_mode == HUB_MODE_STANDALONE ? "단독" : "프록시");
    xTaskCreate(listen_task, "hub_proxy", 6144, NULL, 5, NULL);
}

/* ---------------------------------------------------------------- /key/server (부트스트랩)
 * 응답(XML): <result><error>S000001</error><message>success</message><ip>www.rkiot.net</ip>
 *            <port><mqtt>8883</mqtt>…</port><uid>…</uid><pass>…</pass></result>
 * uid/pass 는 기기마다 다를 수 있으므로 캐시는 (서비스, 기기 MAC)별로, 완전하고 성공인 응답만 저장한다. */
#define BOOT_CAP 2048

/* 캐시 키: "bt" + (서비스|MAC) 의 FNV-1a 32비트(NVS 키는 15자 제한) */
static void boot_key(const char *svc, const char *mac, char key[16])
{
    uint32_t h = 2166136261u;
    for (const char *p = svc; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    h = (h ^ '|') * 16777619u;
    for (const char *p = mac; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    snprintf(key, 16, "bt%08lx", (unsigned long)h);
}

/* 정상 응답인가: 결과 XML 이 끝까지 있고 성공 코드와 접속 정보가 들어 있음 */
static bool boot_valid(const char *body)
{
    return strstr(body, "<result>") && strstr(body, "</result>") && strstr(body, "<error>S000001</error>") &&
           strstr(body, "<uid>") && strstr(body, "<pass>") && strstr(body, "<ip>");
}

static void boot_cache_put(const char *key, const char *body)
{
    nvs_handle_t h;
    size_t n = BOOT_CAP;
    char *old = malloc(n);
    if (old && nvs_open("proxy", NVS_READWRITE, &h) == ESP_OK) {
        if (hs_get_str(h, "proxy", key, old, &n) != ESP_OK || strcmp(old, body)) {   /* 바뀌었을 때만 씀 */
            if (hs_set_str(h, key, body) == ESP_OK) {
                nvs_commit(h);
            }
        }
        nvs_close(h);
    }
    free(old);
}

static bool boot_cache_get(const char *key, char *out, size_t cap)
{
    nvs_handle_t h;
    size_t n = cap;
    bool ok = false;
    if (nvs_open("proxy", NVS_READWRITE, &h) == ESP_OK) {
        ok = hs_get_str(h, "proxy", key, out, &n) == ESP_OK && boot_valid(out);
        nvs_close(h);
    }
    return ok;
}

/* 클라우드에 /key/server 를 다시 묻는다. 돌려주는 값 = HTTP 상태 코드(완전한 응답일 때), -1 = 응답 없음/잘림.
 * *body 는 buf 안의 본문 시작. */
static int boot_fetch(const char *svc, const char *mac, char *buf, int cap, char **body)
{
    *body = NULL;
    if (sta_ready(15)) {
        resolve_up(false);
    }
    int s = tcp_connect(s_up.ip, BOOT_PORT, 8);
    if (s < 0) {
        return -1;
    }
    int n = snprintf(buf, cap, "GET /key/server HTTP/1.0\r\nHost: %s\r\nCk-Service: %s\r\nCk-Mac: %s\r\n\r\n",
                     s_up.host, svc, mac);
    send(s, buf, n, 0);
    int got = 0, r = 0;
    while (got < cap - 1 && (r = recv(s, buf + got, cap - 1 - got, 0)) > 0) {
        got += r;
    }
    close(s);
    buf[got] = '\0';
    if (r < 0 || got >= cap - 1) {
        return -1;                                   /* 시간 초과·오류, 또는 버퍼가 차서 잘렸을 수 있음 */
    }
    int code = 0;
    char *b = strstr(buf, "\r\n\r\n");
    if (!b || sscanf(buf, "HTTP/%*d.%*d %d", &code) != 1) {
        return -1;
    }
    *body = b + 4;
    return code;
}

/* 단독 모드에서 이 기기의 저장된 답이 없을 때: 허브가 답을 만든다. 허브(단독)는 어떤 아이디/암호든 받으므로
 * uid/pass 는 자리 표시. 이 값으로 등록된 기기를 나중에 프록시로 바꾸면 클라우드가 거절할 수 있다
 * (그 기기가 다시 /key/server 를 물으면 진짜 값을 받는다 — 안 되면 버튼 길게로 다시 등록). */
static const char BOOT_LOCAL[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><result><tid></tid><error>S000001</error><message>success</message>"
    "<ip>" UP_HOST "</ip><port><mqtt>8883</mqtt><ws>6002</ws><ssl>true</ssl></port>"
    "<uid>lynnai</uid><pass>standalone</pass></result>";

static esp_err_t boot_send(httpd_req_t *req, int code, const char *body)
{
    /* Pi fakesrv 처럼 헤더+본문을 '한 번에' 보낸다. httpd_resp_send 는 헤더와 본문을 따로 보내서
     * (TCP 조각 2개) GainSpan 이 첫 조각만 읽고 실패 처리하는 것으로 의심됨. */
    int bl = (int)strlen(body);
    char *out = malloc(bl + 160);
    esp_err_t e = ESP_FAIL;
    if (out) {
        int ol = snprintf(out, bl + 160,
                          "HTTP/1.1 %d %s\r\nContent-Type: application/xml; charset=utf-8\r\n"
                          "Content-Length: %d\r\n\r\n%s", code, code == 200 ? "OK" : "Error", bl, body);
        e = httpd_send(req, out, ol) == ol ? ESP_OK : ESP_FAIL;
        free(out);
    }
    /* 다 보낸 뒤 연결을 닫아 응답 끝을 알린다(Pi fakesrv 와 동일). */
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));
    return e;
}

/* 보일러의 GET /key/server
 *   프록시: 클라우드에 다시 묻는다 → 정상(200 + 성공 XML)이면 이 기기 몫으로 저장하고 그대로,
 *           클라우드가 오류로 답하면 그 상태·본문 그대로(저장 안 함),
 *           클라우드가 답을 못 하면(끊김·잘림) 이 기기의 저장된 답, 그것도 없으면 502.
 *   단독  : 허브가 만든 답(BOOT_LOCAL)만 — 저장한 클라우드 계정·암호는 단독에서 내주지 않는다.
 *   허브 AP 대역에서 온 요청만 받는다(집 LAN 쪽은 403). */
esp_err_t hub_proxy_bootstrap_handler(httpd_req_t *req)
{
    char svc[32] = "", mac[32] = "", key[16];
    httpd_req_get_hdr_value_str(req, "Ck-Service", svc, sizeof(svc));
    httpd_req_get_hdr_value_str(req, "Ck-Mac", mac, sizeof(mac));
    boot_key(svc, mac, key);
    bool sa = s_mode == HUB_MODE_STANDALONE;
    /* 기기 전용 망(허브 AP 대역)에서 온 요청만 받는다 — 집 LAN 의 다른 기기가 기기 계정·암호를 받아 가지 못하게 */
    if (!hub_wifi_is_ap_peer(peer_ip(httpd_req_to_sockfd(req)))) {
        ESP_LOGW(TAG, "부트스트랩 거절: 허브 AP 가 아닌 곳에서 온 요청");
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_send(req, "forbidden", HTTPD_RESP_USE_STRLEN);
    }
    ESP_LOGI(TAG, "부트스트랩 요청 (Ck-Mac %s, %s 모드)", mac, sa ? "단독" : "프록시");
    int dev = peer_octet(httpd_req_to_sockfd(req));
    char *buf = malloc(BOOT_CAP);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    esp_err_t e;
    if (!sa) {
        char *body;
        int code = boot_fetch(svc, mac, buf, BOOT_CAP, &body);
        if (code == 200 && boot_valid(body)) {
            boot_cache_put(key, body);
            ESP_LOGI(TAG, "부트스트랩 중계 (%d바이트, 저장)", (int)strlen(body));
            hub_event(EV_BOOTSTRAP, NULL, dev, 1);
            e = boot_send(req, 200, body);
        } else if (code > 0) {
            ESP_LOGW(TAG, "부트스트랩: 클라우드가 %d / 성공 아님 — 그대로 전달(저장 안 함)", code);
            hub_event(EV_BOOTSTRAP, NULL, dev, 0);
            e = boot_send(req, code, body);
        } else if (boot_cache_get(key, buf, BOOT_CAP)) {
            ESP_LOGW(TAG, "부트스트랩: 클라우드 응답 없음 — 이 기기의 저장된 답 사용");
            hub_event(EV_BOOTSTRAP, NULL, dev, 1);
            e = boot_send(req, 200, buf);
        } else {
            ESP_LOGW(TAG, "부트스트랩: 클라우드 응답 없음 + 저장된 답 없음 → 502");
            hub_event(EV_BOOTSTRAP, NULL, dev, 0);
            free(buf);
            httpd_resp_set_status(req, "502 Bad Gateway");
            return httpd_resp_send(req, "upstream", HTTPD_RESP_USE_STRLEN);
        }
    } else {
        /* 단독: 클라우드 계정·암호(프록시 때 저장한 것)는 내주지 않고 허브용 로컬 값만 — 단독 브로커는 무엇이든 받는다 */
        ESP_LOGI(TAG, "부트스트랩(단독): 허브가 만든 답");
        hub_event(EV_BOOTSTRAP, NULL, dev, 1);
        e = boot_send(req, 200, BOOT_LOCAL);
    }
    free(buf);
    return e;
}
