/*
 * hub_devs.c -- 등록된 린나이 기기 목록. 설명은 hub_devs.h.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hub_devs.h"
#include "hub_store.h"
#include "hub_wifi.h"
#include "hub_proxy.h"

static const char *TAG = "HUB_DEVS";

static uint8_t s_reg[HUB_DEVS_MAX][6];
static int s_n;
static SemaphoreHandle_t s_mx;
static int64_t s_auto_until;        /* 이 시각(us)까지 새 기기 자동 등록 */
static bool s_first_run;            /* 등록 목록이 없던 펌웨어에서 올라온 첫 부팅(끝나면 표시를 남긴다) */
static esp_timer_handle_t s_first_timer;

static void lock(void) { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

static int find(const uint8_t mac6[6])
{
    for (int i = 0; i < s_n; i++) {
        if (!memcmp(s_reg[i], mac6, 6)) {
            return i;
        }
    }
    return -1;
}

/* 호출자가 lock 보유 */
static esp_err_t save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open("devs", NVS_READWRITE, &h);
    if (e != ESP_OK) {
        return e;
    }
    e = s_n ? hs_set_blob(h, "list", s_reg, (size_t)s_n * 6) : hs_erase(h, "list");
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        e = ESP_OK;                                  /* 빈 목록인데 지울 것도 없음 */
    }
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

static void first_done(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    if (nvs_open("devs", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, "v", 1) == ESP_OK) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
    s_first_run = false;
    ESP_LOGI(TAG, "첫 부팅 자동 등록 끝 — 등록된 기기 %d대. 이제 새 기기는 버튼 등록이나 관리 화면에서", s_n);
}

void hub_devs_load(void)
{
    s_mx = xSemaphoreCreateMutex();
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("devs", NVS_READWRITE, &h) == ESP_OK) {
        size_t n = sizeof(s_reg);
        esp_err_t e = hs_get_blob(h, "devs", "list", s_reg, &n);
        if (e == ESP_OK && n % 6 == 0) {
            s_n = (int)(n / 6);
        } else if (e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "등록 목록을 읽지 못함(%s) — 빈 목록으로 시작(관리 화면에서 다시 등록)", esp_err_to_name(e));
        }
        nvs_get_u8(h, "v", &v);
        nvs_close(h);
    }
    if (!v) {
        /* 등록 목록이 없던 펌웨어에서 올라옴(또는 새 허브): 쓰던 기기가 다시 붙는 동안은 그대로 등록한다 */
        s_first_run = true;
        s_auto_until = esp_timer_get_time() + (int64_t)HUB_DEVS_FIRST_S * 1000000;
        const esp_timer_create_args_t t = { .callback = first_done, .name = "devs_first" };
        if (esp_timer_create(&t, &s_first_timer) == ESP_OK) {
            esp_timer_start_once(s_first_timer, (uint64_t)HUB_DEVS_FIRST_S * 1000000);
        }
        ESP_LOGW(TAG, "기기 등록 목록 없음 — %d초 동안 허브 AP 에 붙는 기기를 그대로 등록", HUB_DEVS_FIRST_S);
    }
    ESP_LOGI(TAG, "등록된 기기 %d대", s_n);
}

/* 호출자가 lock 보유 */
static esp_err_t add_l(const uint8_t mac6[6])
{
    if (find(mac6) >= 0) {
        return ESP_OK;
    }
    if (s_n >= HUB_DEVS_MAX) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_reg[s_n++], mac6, 6);
    esp_err_t e = save();
    if (e != ESP_OK) {
        s_n--;                                       /* 저장 못 하면 등록하지 않는다(재부팅 뒤 사라지지 않게) */
    }
    return e;
}

esp_err_t hub_devs_add(const uint8_t mac6[6])
{
    if (!s_mx) {
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    esp_err_t e = add_l(mac6);
    unlock();
    return e;
}

bool hub_devs_allowed(const uint8_t mac6[6])
{
    if (!s_mx) {
        return false;
    }
    lock();
    bool ok = find(mac6) >= 0;
    if (!ok && esp_timer_get_time() < s_auto_until) {
        esp_err_t e = add_l(mac6);
        ok = e == ESP_OK;
        if (ok) {
            ESP_LOGW(TAG, "기기 자동 등록(%s): %02x:%02x:%02x:%02x:%02x:%02x", s_first_run ? "첫 부팅" : "버튼 등록", mac6[0],
                     mac6[1], mac6[2], mac6[3], mac6[4], mac6[5]);
        } else {
            ESP_LOGE(TAG, "기기 자동 등록 실패(%s)", esp_err_to_name(e));
        }
    }
    unlock();
    return ok;
}

bool hub_devs_remove3(const uint8_t mac3[3])
{
    if (!s_mx) {
        return false;
    }
    bool any = false;
    lock();
    for (int i = 0; i < s_n;) {
        if (!memcmp(s_reg[i] + 3, mac3, 3)) {
            memmove(s_reg[i], s_reg[i + 1], (size_t)(s_n - i - 1) * 6);
            s_n--;
            any = true;
        } else {
            i++;
        }
    }
    if (any && save() != ESP_OK) {
        ESP_LOGE(TAG, "등록 해제를 저장하지 못함 — 재부팅하면 다시 등록된 것으로 보인다");
    }
    unlock();
    return any;
}

void hub_devs_auto_open(int seconds)
{
    int64_t until = esp_timer_get_time() + (int64_t)seconds * 1000000;
    if (!s_mx) {
        return;
    }
    lock();
    if (until > s_auto_until) {
        s_auto_until = until;
    }
    unlock();
    ESP_LOGI(TAG, "새 기기 자동 등록 %d초", seconds);
}

static int auto_left(void)
{
    int64_t d = s_auto_until - esp_timer_get_time();
    return d > 0 ? (int)(d / 1000000) : 0;
}

static void cat(char *o, size_t cap, const char *s) { strlcat(o, s, cap); }

/* 관리 화면: 허브 AP 에 붙어 있는데 등록 안 된 단말. 린나이 기기가 아닐 수도 있으니 사람이 보고 등록한다 */
void hub_devs_render(char *o, size_t cap)
{
    if (!s_mx) {
        return;
    }
    hub_ap_sta_t sta[10];
    int n = hub_wifi_ap_stations(sta, 10), shown = 0;
    char t[420];
    for (int i = 0; i < n; i++) {
        lock();
        bool reg = find(sta[i].mac) >= 0;
        unlock();
        if (reg) {
            continue;
        }
        if (!shown++) {
            cat(o, cap, "<h2>등록 대기</h2><div class=card><p class=muted>허브 AP 에 붙어 있지만 등록되지 않은 단말입니다. "
                        "등록하기 전에는 허브가 상태를 읽지 않고 SmartThings 에도 보이지 않습니다(프록시 모드면 린나이 앱은 그대로 "
                        "됩니다). 내 기기가 맞을 때만 등록하세요. 등록하면 기기들의 연결이 한 번 끊겼다 다시 붙습니다.</p>");
        }
        const uint8_t *m = sta[i].mac, *p = (const uint8_t *)&sta[i].ip;
        bool rinnai = m[0] == 0x34 && m[1] == 0x5f && m[2] == 0x45;
        snprintf(t, sizeof(t), "<form method=post action=/admin/dev-approve class=row>"
                 "<span><code>%02x:%02x:%02x:%02x:%02x:%02x</code> · %u.%u.%u.%u%s</span>"
                 "<input type=hidden name=approve value=%02x%02x%02x%02x%02x%02x><button class=sv>등록</button></form>",
                 m[0], m[1], m[2], m[3], m[4], m[5], p[0], p[1], p[2], p[3], rinnai ? " · 린나이 Wi-Fi 모듈로 보임" : "",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        cat(o, cap, t);
    }
    int left = auto_left();
    if (shown || left) {
        if (!shown) {
            cat(o, cap, "<h2>등록 대기</h2><div class=card>");
        }
        if (left) {
            snprintf(t, sizeof(t), "<p class=muted>지금은 새로 붙는 기기를 자동으로 등록합니다(%d초 남음 — %s).</p>", left,
                     s_first_run ? "업데이트 후 첫 부팅" : "허브 버튼으로 기기 등록 직후");
            cat(o, cap, t);
        }
        cat(o, cap, "</div>");
    }
}

bool hub_devs_apply(const char *form, char *msg, size_t msglen)
{
    char v[16];
    uint8_t mac[6];
    if (httpd_query_key_value(form, "approve", v, sizeof(v)) != ESP_OK || strlen(v) != 12) {
        snprintf(msg, msglen, "잘못된 요청");
        return false;
    }
    for (int i = 0; i < 6; i++) {
        unsigned b;
        if (sscanf(v + 2 * i, "%2x", &b) != 1) {
            snprintf(msg, msglen, "잘못된 요청");
            return false;
        }
        mac[i] = (uint8_t)b;
    }
    hub_ap_sta_t sta[10];                            /* 지금 허브 AP 에 붙어 있는 단말만 등록한다 */
    int n = hub_wifi_ap_stations(sta, 10);
    bool here = false;
    for (int i = 0; i < n; i++) {
        here |= !memcmp(sta[i].mac, mac, 6);
    }
    if (!here) {
        snprintf(msg, msglen, "그 단말이 지금 허브 AP 에 붙어 있지 않습니다");
        return false;
    }
    esp_err_t e = hub_devs_add(mac);
    if (e != ESP_OK) {
        snprintf(msg, msglen, e == ESP_ERR_NO_MEM ? "등록할 수 있는 기기는 %d대까지입니다" : "등록을 저장하지 못했습니다", HUB_DEVS_MAX);
        return false;
    }
    ESP_LOGW(TAG, "기기 등록(관리 화면·USB): %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    hub_proxy_relink();                              /* 기기 연결을 다시 받아 곧바로 기기로 알아보게 */
    snprintf(msg, msglen, "기기를 등록했습니다. 연결이 다시 붙으면(수십 초) 목록에 나타납니다");
    return true;
}

void hub_devs_print(void)
{
    if (!s_mx) {
        return;
    }
    lock();
    printf("devs registered=%d auto_left=%d", s_n, auto_left());
    for (int i = 0; i < s_n; i++) {
        printf(" %02x:%02x:%02x:%02x:%02x:%02x", s_reg[i][0], s_reg[i][1], s_reg[i][2], s_reg[i][3], s_reg[i][4], s_reg[i][5]);
    }
    printf("\n");
    unlock();
    hub_ap_sta_t sta[10];
    int n = hub_wifi_ap_stations(sta, 10);
    for (int i = 0; i < n; i++) {
        lock();
        bool reg = find(sta[i].mac) >= 0;
        unlock();
        const uint8_t *m = sta[i].mac, *p = (const uint8_t *)&sta[i].ip;
        printf("ap_sta %02x:%02x:%02x:%02x:%02x:%02x %u.%u.%u.%u %s\n", m[0], m[1], m[2], m[3], m[4], m[5], p[0], p[1], p[2],
               p[3], reg ? "registered" : "pending");
    }
}

static bool hexn(const char *s, uint8_t *out, int nbytes)
{
    if ((int)strlen(s) != nbytes * 2) {
        return false;
    }
    for (int i = 0; i < nbytes; i++) {
        unsigned b;
        char hx[3] = { s[2 * i], s[2 * i + 1], 0 };
        if (sscanf(hx, "%2x", &b) != 1) {
            return false;
        }
        out[i] = (uint8_t)b;
    }
    return true;
}

void hub_devs_usb(const char *arg)
{
    uint8_t mac[6];
    while (*arg == ' ') {
        arg++;
    }
    if (!*arg) {
        hub_devs_print();
    } else if (!strncmp(arg, "add ", 4) && hexn(arg + 4, mac, 6)) {
        char form[32], msg[160] = "";
        snprintf(form, sizeof(form), "approve=%s", arg + 4);
        bool ok = hub_devs_apply(form, msg, sizeof(msg));
        printf("#devs add %s: %s\n", ok ? "ok" : "fail", msg);
    } else if (!strncmp(arg, "unreg ", 6) && hexn(arg + 6, mac, 3)) {
        /* 등록만 푼다(허브가 그 기기를 읽지 않게). SmartThings 의 기기는 지우지 않는다 — 지우려면 관리 화면의 기기 삭제 */
        bool ok = hub_devs_remove3(mac);
        if (ok) {
            hub_proxy_relink();
        }
        printf("#devs unreg %s\n", ok ? "ok" : "그런 기기 없음");
    } else {
        printf("#devs | devs add <MAC 12자리> | devs unreg <MAC 끝 6자리>\n");
    }
}
