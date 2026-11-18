/*
 * hub_dns.c -- 허브 AP 쪽 DNS 응답기(UDP 53). Pi 판 standalone/dns_hijack.py + dnsmasq 규칙을 옮긴 것.
 *
 *  - 캡티브(설정 중): 모든 이름의 A 질의를 허브 IP 로 답한다 → 폰 OS 의 인터넷 확인 요청이 허브로 와서
 *    설정 페이지가 자동으로 뜬다.
 *  - 설정 후: 린나이 클라우드 이름(rkiot.net, ckpush.com 및 하위)만 허브로, 나머지는 NXDOMAIN.
 *    보일러가 클라우드 대신 허브에 붙게 하기 위한 것.
 *  AAAA 등 A 가 아닌 질의는 답 없이 NOERROR(IPv6 로 헤매지 않게).
 */
#include <string.h>
#include <strings.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "hub_dns.h"

static const char *TAG = "HUB_DNS";

static volatile bool s_captive = true;
static uint32_t s_ip;                        /* 네트워크 바이트 순서 */
static int s_logged;

static const char *const CLOUD_NAMES[] = { "rkiot.net", "ckpush.com" };

void hub_dns_set_captive(bool captive)
{
    s_captive = captive;
    ESP_LOGI(TAG, "DNS 모드: %s", captive ? "캡티브(모든 이름 → 허브)" : "클라우드 이름만 → 허브");
}

static bool ends_with_domain(const char *name, const char *dom)
{
    size_t n = strlen(name), d = strlen(dom);
    if (n < d || strcasecmp(name + n - d, dom) != 0) {
        return false;
    }
    return n == d || name[n - d - 1] == '.';
}

static bool is_cloud(const char *name)
{
    for (size_t i = 0; i < sizeof(CLOUD_NAMES) / sizeof(CLOUD_NAMES[0]); i++) {
        if (ends_with_domain(name, CLOUD_NAMES[i])) {
            return true;
        }
    }
    return false;
}

/* 질문 이름을 점 표기로 읽는다. 질문 끝 위치(QTYPE 앞)를 돌려준다. 실패 시 -1 */
static int read_qname(const uint8_t *p, int len, int off, char *out, int out_len)
{
    int o = 0;
    while (off < len) {
        uint8_t l = p[off++];
        if (l == 0) {
            out[o] = '\0';
            return off;
        }
        if (l > 63 || off + l > len || o + l + 2 > out_len) {
            return -1;                       /* 압축 포인터는 질문에 오지 않음 — 거부 */
        }
        if (o) {
            out[o++] = '.';
        }
        memcpy(out + o, p + off, l);
        o += l;
        off += l;
    }
    return -1;
}

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "53번 포트 열기 실패");
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    char name[256];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12 || (buf[2] & 0x80) || ((buf[2] >> 3) & 0x0F) != 0) {
            continue;                        /* 너무 짧음 / 응답 패킷 / 표준 질의 아님 */
        }
        uint16_t qd = (buf[4] << 8) | buf[5];
        if (qd < 1) {
            continue;
        }
        int qend = read_qname(buf, n, 12, name, sizeof(name));
        if (qend < 0 || qend + 4 > n) {
            continue;
        }
        uint16_t qtype = (buf[qend] << 8) | buf[qend + 1];
        int len = qend + 4;                  /* 헤더 + 첫 질문까지만 돌려보냄 */

        bool to_hub = s_captive || is_cloud(name);
        uint8_t rcode = to_hub ? 0 : 3;      /* 3 = NXDOMAIN */
        bool answer = to_hub && qtype == 1;  /* A 질의에만 주소로 답 */

        buf[2] = 0x84 | (buf[2] & 0x01);     /* QR=1, AA=1, RD 유지 */
        buf[3] = 0x80 | rcode;               /* RA=1 */
        buf[4] = 0; buf[5] = 1;              /* QDCOUNT=1 */
        buf[6] = 0; buf[7] = answer ? 1 : 0; /* ANCOUNT */
        memset(buf + 8, 0, 4);               /* NSCOUNT, ARCOUNT */
        if (answer) {
            const uint8_t rr[] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x04 };
            memcpy(buf + len, rr, sizeof(rr));   /* 이름=질문 포인터, A, IN, TTL 60, 길이 4 */
            len += sizeof(rr);
            memcpy(buf + len, &s_ip, 4);
            len += 4;
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, fl);
        if (s_logged < 60) {                 /* 처음 60건만: 폰이 무엇을 묻고 무엇을 받았는지 확인용 */
            s_logged++;
            ESP_LOGI(TAG, "[dns] %s type %u -> %s", name, qtype,
                     answer ? "허브" : (rcode ? "NXDOMAIN" : "응답없음"));
        }
    }
}

void hub_dns_start(uint32_t hub_ip_nbo, bool captive)
{
    s_ip = hub_ip_nbo;
    s_captive = captive;
    xTaskCreate(dns_task, "hub_dns", 3584, NULL, 4, NULL);
    const uint8_t *ip = (const uint8_t *)&s_ip;
    ESP_LOGI(TAG, "DNS 시작 (%s), 답하는 허브 IP %u.%u.%u.%u", captive ? "캡티브" : "클라우드 이름만",
             ip[0], ip[1], ip[2], ip[3]);
}
