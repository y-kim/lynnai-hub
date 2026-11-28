/*
 * hub_lan.c -- LAN 통로: SmartThings Edge LAN 드라이버가 집 Wi-Fi 로 허브에 붙는다.
 *
 * 찾기(SSDP): 239.255.255.250:1900 의 M-SEARCH (ST: urn:lynnai-com:device:hub:1 또는 ssdp:all) 에
 *   HTTP/1.1 200 OK + LYNNAI-ID: <허브 id> + LYNNAI-PORT: 8484 로 답한다.
 * 연결(TCP 8484, 줄 단위 텍스트):
 *   [v2 — 토큰을 선에 싣지 않는다(v0.10~)]
 *   드라이버 → 허브  "hello2 <cn> [hub <id> <라벨>]"   cn = 드라이버 난수(32자 16진). 5초 안에
 *   허브 → 드라이버  "chal <sn>"                          sn = 허브 난수
 *   드라이버 → 허브  "auth <cp>"                          cp = HMAC-SHA256(토큰, "c|<cn>|<sn>") 16진 64자
 *   허브 → 드라이버  "ok <허브 id> <sp>" | "denied" | "busy"   sp = HMAC-SHA256(토큰, "s|<sn>|<cn>|<허브 id>")
 *     허브는 저장된 토큰들로 cp 를 맞춰 보고(토큰을 받지 않고 누구인지 앎), 드라이버는 sp 로 진짜 허브인지 확인한다.
 *     드라이버는 한 번 v2 가 통한 허브에는 다시 평문 토큰을 보내지 않는다(허브인 척하는 기기가 'denied'·옛 판 흉내로
 *     토큰을 받아 가지 못하게).
 *   [v1 — 페어링(새 토큰 등록)과 옛 드라이버]
 *   드라이버 → 허브  "hello <토큰 32자 16진> [hub <id> <라벨>]"
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

#include "esp_random.h"
#include "mbedtls/md.h"

#include "hub_lan.h"
#include "hub_uplink.h"
#include "hub_upstream.h"

static const char *TAG = "HUB_LAN";

#define LAN_PORT     8484
#define MAX_CLIENTS  2
#define MAX_TOKENS   4
#define TOKEN_LEN    32
#define LABEL_MAX    24
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

/* 아는 토큰이면 그 칸을 돌려준다. 모르는 토큰은 페어링 창(버튼 짧게 → 3분)이 열려 있을 때만 등록. 거절 = -1 */
static int token_accept(const char *t)
{
    int slot = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (s_tokens[i][0] && same_token(s_tokens[i], t)) {
            slot = i;
        }
    }
    if (slot < 0 && esp_timer_get_time() < s_pair_until_us) {
        int sl = -1;
        for (int i = 0; i < MAX_TOKENS; i++) {
            if (!s_tokens[i][0]) {
                sl = i;
                break;
            }
        }
        if (sl < 0) {                                /* 꽉 참: 가장 오래된 칸부터 돌아가며 */
            sl = s_next;
            s_next = (s_next + 1) % MAX_TOKENS;
        }
        char old[TOKEN_LEN + 1];
        strcpy(old, s_tokens[sl]);
        strcpy(s_tokens[sl], t);
        if (tokens_save() == ESP_OK) {
            slot = sl;
            ESP_LOGI(TAG, "새 허브/드라이버 등록(칸 %d)", sl);
        } else {
            strcpy(s_tokens[sl], old);               /* 저장 못 하면 등록하지 않는다(재부팅 뒤 사라지지 않게) */
            ESP_LOGE(TAG, "새 토큰 저장 실패");
        }
    }
    xSemaphoreGive(s_lock);
    return slot;
}

void hub_lan_token_remove(int slot)
{
    if (slot < 0 || slot >= MAX_TOKENS) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tokens[slot][0] = 0;
    tokens_save();
    xSemaphoreGive(s_lock);
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

static bool client_room(void)                         /* s_clients 에 빈 칸이 있나 */
{
    bool room = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS && !room; i++) {
        room = s_clients[i] < 0;
    }
    xSemaphoreGive(s_lock);
    return room;
}

static int client_add(int fd)                         /* s_clients 빈 칸에 넣는다(= hub_lan_send 가 뿌릴 대상) */
{
    int slot = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i] < 0) {
            s_clients[i] = fd;
            slot = i;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return slot;
}

static void client_del(int slot)
{
    if (slot < 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_clients[slot] = -1;
    xSemaphoreGive(s_lock);
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

/* ---------------------------------------------------------------- v2 인증(HMAC) */

static void to_hex(const uint8_t *b, int n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[2 * i] = hx[b[i] >> 4];
        out[2 * i + 1] = hx[b[i] & 15];
    }
    out[2 * n] = 0;
}

static bool is_hex(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return s[n] == 0 || s[n] == ' ';
}

/* HMAC-SHA256(key, a|b|c|d) → 16진 64자(빈 조각은 건너뜀). 조각 사이는 '|' */
static void hmac_hex(const char *key, const char *a, const char *b, const char *c, const char *d, char out[65])
{
    uint8_t mac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
    const char *parts[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) {
        if (!parts[i]) continue;
        if (i) mbedtls_md_hmac_update(&ctx, (const unsigned char *)"|", 1);
        mbedtls_md_hmac_update(&ctx, (const unsigned char *)parts[i], strlen(parts[i]));
    }
    mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);
    to_hex(mac, 32, out);
}

static bool same_hex64(const char *a, const char *b)    /* 같은 시간 비교 */
{
    uint8_t d = 0;
    for (int i = 0; i < 64; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0 && b[64] == 0;
}

/* cp 가 저장된 토큰 가운데 하나로 만든 것이면 그 칸, 아니면 -1 */
static int token_by_proof(const char *cn, const char *sn, const char *cp)
{
    if (strlen(cp) != 64) return -1;
    int found = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_TOKENS; i++) {
        if (!s_tokens[i][0]) continue;
        char want[65];
        hmac_hex(s_tokens[i], "c", cn, sn, NULL, want);
        if (same_hex64(want, cp) && found < 0) found = i;
    }
    xSemaphoreGive(s_lock);
    return found;
}

static void server_proof(int slot, const char *sn, const char *cn, char out[65])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char tok[TOKEN_LEN + 1];
    strlcpy(tok, s_tokens[slot], sizeof(tok));
    xSemaphoreGive(s_lock);
    const char *pre = "s";
    char sn_cn[80];
    snprintf(sn_cn, sizeof(sn_cn), "%s|%s", sn, cn);
    hmac_hex(tok, pre, sn_cn, s_id, NULL, out);
    memset(tok, 0, sizeof(tok));
}

/* "hub <id> <라벨>" 꼬리를 푼다 */
static void parse_hub_tail(const char *extra, bool *is_hub, char *gid, size_t gidn, char *glabel, size_t gln)
{
    if (!extra || strncmp(extra, "hub", 3) || (extra[3] != ' ' && extra[3] != 0)) return;
    *is_hub = true;
    const char *p = extra + 3;
    while (*p == ' ') p++;
    const char *sp2 = strchr(p, ' ');
    if (sp2) {
        int idn = (int)(sp2 - p);
        if (idn > (int)gidn - 1) idn = gidn - 1;
        memcpy(gid, p, idn);
        gid[idn] = 0;
        const char *lp = sp2 + 1;
        while (*lp == ' ') lp++;
        snprintf(glabel, gln, "%s", lp);
    } else {
        snprintf(gid, gidn, "%s", p);
    }
}

/* ---------------------------------------------------------------- 받기 */

static int hexval(char c) { return isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10); }

typedef struct { int fd; char ip[40]; } conn_arg_t;

/* "<cmd 2hex> <payload hex>" → cmd + 바이트. 성공 시 바이트 수(≥0), 실패 -1 */
static int lan_parse(const char *line, int *cmd, uint8_t *buf, int cap)
{
    if (strlen(line) < 2 || !isxdigit((unsigned char)line[0]) || !isxdigit((unsigned char)line[1])) {
        return -1;
    }
    *cmd = hexval(line[0]) * 16 + hexval(line[1]);
    const char *hx = line[2] == ' ' ? line + 3 : line + 2;
    int n = 0;
    for (; hx[0] && hx[1] && n < cap; hx += 2) {
        if (!isxdigit((unsigned char)hx[0]) || !isxdigit((unsigned char)hx[1])) {
            return -1;
        }
        buf[n++] = (uint8_t)(hexval(hx[0]) * 16 + hexval(hx[1]));
    }
    return n;
}

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
    conn_arg_t *ca = (conn_arg_t *)arg;
    int fd = ca->fd;
    char peer_ip[40];
    snprintf(peer_ip, sizeof(peer_ip), "%s", ca->ip);
    free(ca);

    char rx[512];
    int len = 0, slot = -1, gi = -1;
    bool authed = false, pre = true, consumer = false;   /* consumer = 중심 모드에서 위성이 올려 주는 연결 */
    char cn[33] = "", sn[33] = "";                     /* v2 난수(드라이버·허브) — chal 을 보냈으면 sn 이 차 있다 */
    char gid[16] = "", glabel[LABEL_MAX + 1] = "";
    bool is_hub = false;
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
                if (!authed && !sn[0] && !strncmp(rx, "hello2 ", 7) && is_hex(rx + 7, 32)) {
                    /* v2 첫 줄: 드라이버 난수를 받고 허브 난수로 답한다(토큰은 안 오간다) */
                    memcpy(cn, rx + 7, 32);
                    cn[32] = 0;
                    for (char *q = cn; *q; q++) *q = (char)tolower((unsigned char)*q);
                    parse_hub_tail(rx[39] == ' ' ? rx + 40 : NULL, &is_hub, gid, sizeof(gid), glabel, sizeof(glabel));
                    uint8_t r[16];
                    esp_fill_random(r, sizeof(r));
                    to_hex(r, 16, sn);
                    char ch[48];
                    int k = snprintf(ch, sizeof(ch), "chal %s\n", sn);
                    send_line(fd, ch, k);
                } else if (!authed) {
                    /* v2 둘째 줄 "auth <cp>", 또는 v1 "hello <토큰> [hub <id> <라벨>]"(페어링·옛 드라이버) */
                    bool ok_tok = false, v2 = false;
                    int tokslot = -1;
                    if (sn[0] && !strncmp(rx, "auth ", 5)) {
                        v2 = true;
                        tokslot = token_by_proof(cn, sn, rx + 5);
                        ok_tok = tokslot >= 0;
                    } else if (!sn[0] && !strncmp(rx, "hello ", 6)) {
                        char *tp = rx + 6;
                        char *sp = strchr(tp, ' ');
                        char *extra = NULL;
                        if (sp) {
                            *sp = 0;
                            extra = sp + 1;
                        }
                        if (token_ok(tp)) {
                            tokslot = token_accept(tp);
                            ok_tok = tokslot >= 0;
                        }
                        parse_hub_tail(extra, &is_hub, gid, sizeof(gid), glabel, sizeof(glabel));
                    }
                    if (!ok_tok) {
                        vTaskDelay(pdMS_TO_TICKS(1000));   /* 토큰 맞히기 늦추기 */
                        send_line(fd, "denied\n", 7);
                        ESP_LOGW(TAG, "토큰 거절(페어링하려면 버튼 짧게)");
                        goto out;
                    }
                    consumer = is_hub && hub_upstream_mode() == HUB_UP_HOST;
                    char ok[112];
                    int k;
                    if (v2) {
                        char sp[65];
                        server_proof(tokslot, sn, cn, sp);
                        k = snprintf(ok, sizeof(ok), "ok %s %s\n", s_id, sp);
                    } else {
                        k = snprintf(ok, sizeof(ok), "ok %s\n", s_id);
                    }
                    if (consumer) {
                        gi = hub_upstream_guest_join(fd, tokslot, gid, glabel, peer_ip);
                        if (gi < 0) {
                            send_line(fd, "busy\n", 5);
                            goto out;
                        }
                        send_line(fd, ok, k);
                    } else {
                        if (!client_room()) {            /* 빈 칸부터 — ok 뒤에 busy 를 보내면 상대가 붙은 줄 안다 */
                            send_line(fd, "busy\n", 5);
                            goto out;
                        }
                        send_line(fd, ok, k);            /* 등록 전에 보내 다른 줄과 섞이지 않게 */
                        slot = client_add(fd);
                        if (slot < 0) {                  /* 그 사이 다른 연결이 칸을 가져감(드묾) — 끊는다 */
                            goto out;
                        }
                    }
                    authed = true;
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    s_preauth--;
                    pre = false;
                    xSemaphoreGive(s_lock);
                    tv.tv_sec = 30;                  /* 이후엔 30초마다 깨어 ping */
                    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                    ESP_LOGI(TAG, "%s", consumer ? "위성 허브 연결됨" : "드라이버 연결됨");
                    if (!consumer) {
                        hub_uplink_on_request(HUB_UPLINK_LAN, "refresh", 7);   /* 지금 상태를 곧바로 */
                    }
                } else if (consumer) {
                    int cmd;
                    uint8_t buf[HUB_BLOB_MAX + 8];
                    int bn = lan_parse(rx, &cmd, buf, sizeof(buf));
                    if (bn >= 0) {
                        hub_upstream_guest_frame(gi, (uint8_t)cmd, buf, bn);
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
    if (slot >= 0) {
        client_del(slot);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (pre) {
        s_preauth--;
    }
    xSemaphoreGive(s_lock);
    if (gi >= 0) {
        hub_upstream_guest_leave(gi, fd);            /* 먼저 표에서 — 닫은 뒤 다른 연결이 같은 번호를 받을 수 있다 */
    }
    close(fd);
    if (authed) {
        ESP_LOGI(TAG, "%s", consumer ? "위성 허브 연결 끊김" : "드라이버 연결 끊김");
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
        conn_arg_t *ca = malloc(sizeof(conn_arg_t));
        if (ca) {
            ca->fd = fd;
            snprintf(ca->ip, sizeof(ca->ip), "%s", inet_ntoa(peer.sin_addr));
        }
        if (!ca || xTaskCreate(client_task, "hub_lan_c", 4096, ca, 4, NULL) != pdPASS) {
            free(ca);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_preauth--;
            xSemaphoreGive(s_lock);
            close(fd);
        }
    }
}

/* ---------------------------------------------------------------- 위성 다이얼러(전화 거는 쪽) */

static volatile bool s_guest_kick;
static void guest_v2_mark(bool on);
static volatile bool s_guest_online;
static char s_guest_status[48] = "꺼짐";

void hub_lan_guest_reconnect(void) { s_guest_kick = true; }
/* 관리 화면 '중심 허브에 연결하기'(사람이 누름): v2 로 붙어 본 기록을 지워, 중심이 토큰을 모르면(중심 초기화·삭제 뒤)
 * 다시 페어링(v1)할 수 있게 한다. 자동 재접속은 이 기록을 지우지 않는다. */
void hub_lan_guest_repair(void)    { guest_v2_mark(false); s_guest_kick = true; }
bool hub_lan_guest_online(void)    { return s_guest_online; }
const char *hub_lan_guest_status(void) { return s_guest_status; }

static void gstatus(const char *s) { snprintf(s_guest_status, sizeof(s_guest_status), "%s", s); }

/* 위성 쪽 v2: 한 번 v2 로 붙은 중심에는 다시 평문 토큰(v1)을 보내지 않는다 — NVS "lan"/"gv2" */
static bool guest_v2_seen(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("lan", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "gv2", &v);
        nvs_close(h);
    }
    return v == 1;
}

static void guest_v2_mark(bool on)
{
    nvs_handle_t h;
    if (nvs_open("lan", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "gv2", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool s_guest_try_v1;                 /* 다음 한 번은 v1(페어링·옛 중심) — v2 를 본 적 없을 때만 켜진다 */

static int guest_dial(const char *ip, const char *tok, bool v2, char cn[33])
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(LAN_PORT) };
    if (inet_aton(ip, &a.sin_addr) != 1) {
        gstatus("IP 형식 오류");
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }
    struct timeval tv = { .tv_sec = 8 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        gstatus("접속 실패(IP 확인)");
        close(fd);
        return -1;
    }
    char label[LABEL_MAX + 1];
    hub_label_get(label, sizeof(label));
    char hello[160];
    int k;
    if (v2) {
        uint8_t r[16];
        esp_fill_random(r, sizeof(r));
        to_hex(r, 16, cn);
        k = snprintf(hello, sizeof(hello), "hello2 %s hub %s %s\n", cn, s_id, label);
    } else {
        k = snprintf(hello, sizeof(hello), "hello %s hub %s %s\n", tok, s_id, label);
    }
    if (send(fd, hello, k, 0) != k) {
        close(fd);
        return -1;
    }
    return fd;
}

static void guest_task(void *arg)
{
    char rx[512];
    for (;;) {
        char ip[40], tok[40];
        if (!hub_upstream_guest_target(ip, sizeof(ip), tok, sizeof(tok))) {
            s_guest_online = false;
            gstatus(hub_upstream_mode() == HUB_UP_GUEST ? "중심 허브 IP 를 넣으세요" : "꺼짐");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        s_guest_kick = false;
        gstatus("연결 중…");
        bool v2 = !s_guest_try_v1;
        s_guest_try_v1 = false;
        char cn[33] = "", sn[33] = "";
        bool chal = false;
        int fd = guest_dial(ip, tok, v2, cn);
        if (fd < 0) {
            s_guest_online = false;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        int len = 0, slot = -1;
        bool authed = false;
        int64_t last_rx = esp_timer_get_time();
        for (;;) {
            if (s_guest_kick || hub_upstream_mode() != HUB_UP_GUEST) {
                break;
            }
            int r = recv(fd, rx + len, sizeof(rx) - 1 - len, 0);
            if (r == 0) {
                break;
            }
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (esp_timer_get_time() - last_rx > 90LL * 1000000) {   /* 중심 ping 은 30초 — 90초면 끊긴 것 */
                        break;
                    }
                    continue;
                }
                break;
            }
            last_rx = esp_timer_get_time();
            len += r;
            rx[len] = 0;
            char *nl;
            while ((nl = strchr(rx, '\n'))) {
                *nl = 0;
                if (nl > rx && nl[-1] == '\r') {
                    nl[-1] = 0;
                }
                if (!authed && v2 && !chal && !strncmp(rx, "chal ", 5) && is_hex(rx + 5, 32)) {
                    memcpy(sn, rx + 5, 32);
                    sn[32] = 0;
                    for (char *q = sn; *q; q++) *q = (char)tolower((unsigned char)*q);
                    chal = true;
                    char cp[65], line[80];
                    hmac_hex(tok, "c", cn, sn, NULL, cp);
                    int k = snprintf(line, sizeof(line), "auth %s\n", cp);
                    if (send(fd, line, k, 0) != k) {
                        goto gdrop;
                    }
                } else if (!authed) {
                    if (!strncmp(rx, "ok ", 3)) {
                        if (v2) {                            /* 중심이 토큰을 아는지(진짜인지) 확인: "ok <id> <sp>" */
                            const char *sp = strrchr(rx, ' ');
                            char cid[16] = "", want[65], sn_cn[80];
                            const char *idp = rx + 3, *ide = strchr(idp, ' ');
                            if (ide && ide - idp < (int)sizeof(cid)) {
                                memcpy(cid, idp, ide - idp);
                            }
                            snprintf(sn_cn, sizeof(sn_cn), "%s|%s", sn, cn);
                            hmac_hex(tok, "s", sn_cn, cid, NULL, want);
                            if (!chal || !sp || !cid[0] || !same_hex64(want, sp + 1)) {
                                gstatus("중심 허브 확인 실패(토큰 증명이 맞지 않음)");
                                ESP_LOGW(TAG, "중심 허브의 증명이 맞지 않음 — 끊음");
                                goto gdrop;
                            }
                            if (!guest_v2_seen()) {
                                guest_v2_mark(true);
                            }
                        }
                        authed = true;
                        slot = client_add(fd);               /* 이제 hub_lan_send 가 우리 STATE/LINKS 를 중심으로 보냄 */
                        s_guest_online = true;
                        gstatus("연결됨");
                        ESP_LOGI(TAG, "중심 허브 연결됨 (%s)", ip);
                        hub_uplink_on_request(HUB_UPLINK_LAN, "refresh", 7);   /* 지금 상태를 바로 올린다 */
                    } else if (!strncmp(rx, "denied", 6)) {
                        /* v2 거절 = 중심이 이 토큰을 모름(아직 페어링 전). v2 로 붙어 본 적이 없을 때만 다음은 v1(등록) —
                         * 본 적 있으면 중심인 척하는 기기일 수 있으니 평문 토큰을 보내지 않는다 */
                        if (v2 && !guest_v2_seen()) {
                            s_guest_try_v1 = true;
                        }
                        gstatus("거절됨 — 중심에서 '연결 허용'을 누르세요");
                        goto gdrop;
                    } else if (!strncmp(rx, "busy", 4)) {
                        gstatus("중심 허브 연결 꽉 참");
                        goto gdrop;
                    }
                } else if (strcmp(rx, "ping")) {             /* ping 은 무시, 나머지는 명령 */
                    handle_line(rx);                         /* 중심이 보낸 REQUEST → 우리 버스에 적용 */
                }
                len -= (nl + 1 - rx);
                memmove(rx, nl + 1, len + 1);
            }
            if (len >= (int)sizeof(rx) - 1) {
                len = 0;
            }
        }
        if (v2 && !chal && !authed && !guest_v2_seen()) {
            s_guest_try_v1 = true;                   /* chal 없이 닫힘 = hello2 를 모르는 옛 중심 펌웨어 */
        }
    gdrop:
        if (slot >= 0) {
            client_del(slot);
        }
        close(fd);
        s_guest_online = false;
        if (authed) {
            gstatus("끊김 — 다시 시도");
            ESP_LOGW(TAG, "중심 허브 연결 끊김");
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
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
    xTaskCreate(guest_task, "hub_lan_g", 4096, NULL, 4, NULL);  /* 위성 모드일 때 중심 허브로 전화를 건다 */
    xTaskCreate(ssdp_task, "hub_ssdp", 4096, NULL, 3, NULL);   /* 3072 일 때 스택이 444B 밖에 안 남았다(실측) */
}
