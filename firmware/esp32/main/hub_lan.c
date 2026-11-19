/*
 * hub_lan.c -- LAN 통로: SmartThings Edge LAN 드라이버가 집 Wi-Fi 로 허브에 붙는다.
 *
 * 찾기(SSDP): 239.255.255.250:1900 의 M-SEARCH (ST: urn:lynnai-com:device:hub:1 또는 ssdp:all) 에
 *   HTTP/1.1 200 OK + LYNNAI-ID: <허브 id> + LYNNAI-PORT: 8484 로 답한다.
 * 연결(TCP 8484, 줄 단위 텍스트):
 *   드라이버 → 허브  "hello <토큰 32자 16진>"   먼저 보내야 한다(5초 안)
 *   허브 → 드라이버  "ok <허브 id>" 또는 "denied"
 *   이후 양쪽 다     "<명령 16진 2자리> <내용 16진>"   (명령 번호·내용은 hub_uplink.h, Zigbee 와 같음)
 *   허브 → 드라이버  "ping" 30초마다(드라이버가 끊김을 알아채게)
 * 토큰(페어링): 새 드라이버는 허브 버튼을 짧게 누른 뒤 3분 안에만 등록된다(첫 설치도 같음 — 집 LAN 의 아무 기기나
 *   먼저 붙어 등록되지 않게). 최대 4개, 꽉 차면 가장 오래 전에 등록한 것부터 바꾼다.
 * 인증 전 연결은 동시에 2개까지, 5초 안에 hello 가 없으면 끊는다. 보내기는 기다리지 않는다(읽지 않는 상대는 끊음).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "nvs.h"
#include "hub_store.h"
#include "lwip/sockets.h"
#include "lwip/igmp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "hub_lan.h"
#include "hub_uplink.h"

static const char *TAG = "HUB_LAN";

#define LAN_PORT     8484
#define MAX_CLIENTS  2
#define MAX_TOKENS   4
#define TOKEN_LEN    32
#define SSDP_ST      "urn:lynnai-com:device:hub:1"

static char s_id[16];
static SemaphoreHandle_t s_lock;
static int s_clients[MAX_CLIENTS] = { -1, -1 };     /* 인증된 소켓 */
static char s_tokens[MAX_TOKENS][TOKEN_LEN + 1];
static int64_t s_pair_until_us;
static int s_next;                                   /* 꽉 찼을 때 다음에 바꿀 칸(가장 오래된 것) */
static int s_preauth;                                /* 인증 전 연결 수 */
#define MAX_PREAUTH  2

/* ---------------------------------------------------------------- 토큰 */

static void tokens_load(void)
{
    nvs_handle_t h;
    if (nvs_open("lan", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    for (int i = 0; i < MAX_TOKENS; i++) {
        char key[4] = { 't', (char)('0' + i), 0 };
        size_t n = TOKEN_LEN + 1;
        esp_err_t e = hs_get_str(h, "lan", key, s_tokens[i], &n);
        if (e != ESP_OK) {
            s_tokens[i][0] = 0;
            if (e != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGE(TAG, "토큰 %d 읽기 실패(%s) — 그 드라이버는 다시 페어링 필요", i, esp_err_to_name(e));
            }
        }
    }
    uint8_t nx = 0;
    nvs_get_u8(h, "nx", &nx);
    s_next = nx % MAX_TOKENS;
    nvs_close(h);
}

static esp_err_t tokens_save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("lan", NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    for (int i = 0; e == ESP_OK && i < MAX_TOKENS; i++) {
        char key[4] = { 't', (char)('0' + i), 0 };
        e = hs_set_str(h, key, s_tokens[i]);
    }
    if (e == ESP_OK) {
        e = nvs_set_u8(h, "nx", (uint8_t)s_next);
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

static bool same_token(const char *a, const char *b)   /* 길이 고정(32) — 시간이 내용에 따라 달라지지 않게 */
{
    uint8_t d = 0;
    for (int i = 0; i < TOKEN_LEN; i++) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0;
}

static bool token_ok(const char *t)
{
    if (strlen(t) != TOKEN_LEN) {
        return false;
    }
    for (int i = 0; i < TOKEN_LEN; i++) {
        if (!isxdigit((unsigned char)t[i])) {
            return false;
        }
    }
    return true;
}

/* 아는 토큰이면 true. 모르는 토큰은 페어링 창(버튼 짧게 → 3분)이 열려 있을 때만 등록 */
static bool token_accept(const char *t)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (s_tokens[i][0] && same_token(s_tokens[i], t)) {
            ok = true;
        }
    }
    if (!ok && esp_timer_get_time() < s_pair_until_us) {
        int slot = -1;
        for (int i = 0; i < MAX_TOKENS; i++) {
            if (!s_tokens[i][0]) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {                              /* 꽉 참: 가장 오래된 칸부터 돌아가며 */
            slot = s_next;
            s_next = (s_next + 1) % MAX_TOKENS;
        }
        char old[TOKEN_LEN + 1];
        strcpy(old, s_tokens[slot]);
        strcpy(s_tokens[slot], t);
        if (tokens_save() == ESP_OK) {
            ok = true;
            ESP_LOGI(TAG, "새 드라이버 등록(칸 %d)", slot);
        } else {
            strcpy(s_tokens[slot], old);             /* 저장 못 하면 등록하지 않는다(재부팅 뒤 사라지지 않게) */
            ESP_LOGE(TAG, "새 드라이버 토큰 저장 실패");
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void hub_lan_pairing_open(int seconds)
{
    s_pair_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    ESP_LOGI(TAG, "LAN 페어링 %d초 열림", seconds);
}

/* ---------------------------------------------------------------- 보내기 */

/* 기다리지 않고 보낸다: 상대가 읽지 않아 버퍼가 찼으면 그 연결을 끊는다(다른 통로·Zigbee 전송이 멈추지 않게) */
static void send_line(int fd, const char *line, int n)
{
    if (send(fd, line, n, MSG_DONTWAIT) != n) {
        shutdown(fd, SHUT_RDWR);                      /* 받는 태스크가 정리한다 */
    }
}

void hub_lan_send(uint8_t cmd, const uint8_t *data, uint16_t n)
{
    if (!s_lock || n > 200) {
        return;
    }
    char line[3 + 2 * 200 + 2];
    int k = snprintf(line, sizeof(line), "%02x ", cmd);
    for (int i = 0; i < n; i++) {
        k += snprintf(line + k, sizeof(line) - k, "%02x", data[i]);
    }
    line[k++] = '\n';
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i] >= 0) {
            send_line(s_clients[i], line, k);
        }
    }
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- 받기 */

static int hexval(char c) { return isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10); }

static void handle_line(char *line)
{
    /* "<cmd> <hex>" */
    if (strlen(line) < 3 || !isxdigit((unsigned char)line[0]) || !isxdigit((unsigned char)line[1])) {
        return;
    }
    int cmd = hexval(line[0]) * 16 + hexval(line[1]);
    char *hx = line[2] == ' ' ? line + 3 : line + 2;
    char buf[160];
    int n = 0;
    for (; hx[0] && hx[1] && n < (int)sizeof(buf); hx += 2) {
        if (!isxdigit((unsigned char)hx[0]) || !isxdigit((unsigned char)hx[1])) {
            return;
        }
        buf[n++] = (char)(hexval(hx[0]) * 16 + hexval(hx[1]));
    }
    if (cmd == HUB_RK_CMD_REQUEST && n > 0) {
        hub_uplink_on_request(HUB_UPLINK_LAN, buf, n);
    }
}

static void client_task(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char rx[512];
    int len = 0, slot = -1;
    bool authed = false, pre = true;                  /* pre = 인증 전 연결 수에 들어가 있음 */
    struct timeval tv = { .tv_sec = 1 };              /* 인증 전엔 1초마다 깨어 5초 기한을 본다 */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int64_t t0 = esp_timer_get_time(), last_ping = t0;

    for (;;) {
        if (!authed && esp_timer_get_time() - t0 > 5LL * 1000000) {   /* 조금씩 흘려 보내도 5초면 끝 */
            ESP_LOGW(TAG, "5초 안에 hello 없음 — 끊음");
            break;
        }
        int r = recv(fd, rx + len, sizeof(rx) - 1 - len, 0);
        if (r == 0 || (r < 0 && (errno != EAGAIN && errno != EWOULDBLOCK))) {
            break;
        }
        if (r > 0) {
            len += r;
            rx[len] = 0;
            char *nl;
            while ((nl = strchr(rx, '\n'))) {
                *nl = 0;
                if (nl > rx && nl[-1] == '\r') {
                    nl[-1] = 0;
                }
                if (!authed) {
                    if (!strncmp(rx, "hello ", 6) && token_ok(rx + 6) && token_accept(rx + 6)) {
                        char ok[32];
                        int k = snprintf(ok, sizeof(ok), "ok %s\n", s_id);
                        xSemaphoreTake(s_lock, portMAX_DELAY);
                        for (int i = 0; i < MAX_CLIENTS; i++) {
                            if (s_clients[i] < 0) {
                                slot = i;
                                break;
                            }
                        }
                        if (slot >= 0) {
                            send_line(fd, ok, k);                /* 등록 전에 보내 다른 줄과 섞이지 않게 */
                            s_clients[slot] = fd;
                        }
                        xSemaphoreGive(s_lock);
                        if (slot < 0) {
                            send_line(fd, "busy\n", 5);
                            goto out;
                        }
                        authed = true;
                        xSemaphoreTake(s_lock, portMAX_DELAY);
                        s_preauth--;
                        pre = false;
                        xSemaphoreGive(s_lock);
                        tv.tv_sec = 30;                  /* 이후엔 30초마다 깨어 ping */
                        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                        ESP_LOGI(TAG, "드라이버 연결됨");
                        hub_uplink_on_request(HUB_UPLINK_LAN, "refresh", 7);   /* 지금 상태를 곧바로 */
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(1000));   /* 토큰 맞히기 늦추기 */
                        send_line(fd, "denied\n", 7);
                        ESP_LOGW(TAG, "토큰 거절(페어링하려면 버튼 짧게)");
                        goto out;
                    }
                } else {
                    handle_line(rx);
                }
                len -= (nl + 1 - rx);
                memmove(rx, nl + 1, len + 1);
            }
            if (len >= (int)sizeof(rx) - 1) {          /* 너무 긴 줄: 인증 전이면 끊고, 뒤면 버림 */
                if (!authed) {
                    break;
                }
                len = 0;
            }
        }
        if (authed && esp_timer_get_time() - last_ping > 30LL * 1000000) {
            last_ping = esp_timer_get_time();
            xSemaphoreTake(s_lock, portMAX_DELAY);
            send_line(fd, "ping\n", 5);
            xSemaphoreGive(s_lock);
        }
    }
out:
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (slot >= 0) {
        s_clients[slot] = -1;
    }
    if (pre) {
        s_preauth--;
    }
    xSemaphoreGive(s_lock);
    close(fd);
    if (authed) {
        ESP_LOGI(TAG, "드라이버 연결 끊김");
    }
    vTaskDelete(NULL);
}

static void server_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(LAN_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 2) < 0) {
        ESP_LOGE(TAG, "TCP %d 열기 실패", LAN_PORT);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "LAN 통로 대기: TCP %d", LAN_PORT);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int fd = accept(ls, (struct sockaddr *)&peer, &pl);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool room = s_preauth < MAX_PREAUTH;
        if (room) {
            s_preauth++;
        }
        xSemaphoreGive(s_lock);
        if (!room) {                                  /* 인증 전 연결이 이미 많음 — 소켓 고갈 막기 */
            close(fd);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        int ka = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
        if (xTaskCreate(client_task, "hub_lan_c", 4096, (void *)(intptr_t)fd, 4, NULL) != pdPASS) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_preauth--;
            xSemaphoreGive(s_lock);
            close(fd);
        }
    }
}

/* ---------------------------------------------------------------- SSDP */

static bool sta_ip(esp_ip4_addr_t *ip)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!n || esp_netif_get_ip_info(n, &info) != ESP_OK || !info.ip.addr) {
        return false;
    }
    *ip = info.ip;
    return true;
}

static void ssdp_task(void *arg)
{
    int s = -1;
    bool joined = false;
    char buf[512];
    for (;;) {
        esp_ip4_addr_t ip;
        if (!sta_ip(&ip)) {                           /* 집 Wi-Fi IP 가 생길 때까지 */
            if (s >= 0) {
                close(s);
                s = -1;
                joined = false;
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        if (s < 0) {
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            int one = 1;
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(1900), .sin_addr.s_addr = htonl(INADDR_ANY) };
            if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
                close(s);
                s = -1;
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
            struct timeval tv = { .tv_sec = 5 };
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
        if (!joined) {
            struct ip_mreq m = { .imr_multiaddr.s_addr = inet_addr("239.255.255.250"), .imr_interface.s_addr = ip.addr };
            joined = setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) == 0;
            if (joined) {
                ESP_LOGI(TAG, "SSDP 응답 준비 (" IPSTR ")", IP2STR(&ip));
            }
        }
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;
        }
        buf[n] = 0;
        if (strncmp(buf, "M-SEARCH", 8) || (!strstr(buf, SSDP_ST) && !strstr(buf, "ssdp:all"))) {
            continue;
        }
        char rsp[400];
        int k = snprintf(rsp, sizeof(rsp),
                         "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                         "LOCATION: http://" IPSTR "/\r\nSERVER: ESP32 UPnP/1.1 LYNNAI/1\r\n"
                         "ST: " SSDP_ST "\r\nUSN: uuid:lynnai-%s::" SSDP_ST "\r\n"
                         "LYNNAI-ID: %s\r\nLYNNAI-PORT: %d\r\n\r\n",
                         IP2STR(&ip), s_id, s_id, LAN_PORT);
        sendto(s, rsp, k, 0, (struct sockaddr *)&from, fl);
    }
}

/* ---------------------------------------------------------------- 시작 */

void hub_lan_start(const char *device_id)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();
    snprintf(s_id, sizeof(s_id), "%s", device_id);
    tokens_load();
    xTaskCreate(server_task, "hub_lan", 3072, NULL, 4, NULL);
    xTaskCreate(ssdp_task, "hub_ssdp", 3072, NULL, 3, NULL);
}
