/*
 * hub_diag.c -- 고장 진단 두 갈래.
 *
 *  1) LED 오류 코드(평소): 5초마다 상태를 보고 hub_led_codes() 로 빨강 N번 깜빡임을 정한다.
 *       1 집 Wi-Fi 끊김(설정돼 있는데 2분 넘게, Zigbee 가입 중 일시 중단은 제외)
 *       2 Zigbee 미가입(부팅 90초 뒤에도)            3 린나이 기기 링크 끊김(5분 넘게)
 *       4 저장 데이터 오류(hub_store 가 래치)
 *  2) USB 시리얼(필요할 때): UART0(보드의 'UART' USB 포트, 115200) 에 "diag" 한 줄을 보내면
 *     #DIAG-BEGIN ~ #DIAG-END 사이에 상태 전체를 출력한다. PC 쪽은 tools/hub_doctor.py --serial.
 *
 * 비밀값은 내보내지 않는다: 집 Wi-Fi·허브 AP 암호, 관리자 암호 해시, 서버 API 토큰, 클라우드 인증서/키.
 * 그 밖의 진단 정보(SSID, IP, MAC, 이벤트, 오류)는 그대로 — 고장 원인을 찾는 데 필요하다.
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"

#include "hub_diag.h"
#include "hub_led.h"
#include "hub_wifi.h"
#include "hub_rinnai.h"
#include "hub_store.h"
#include "hub_events.h"
#include "hub_uplink.h"
#include "hub_zb_rinnai.h"
#include "hub_proxy.h"
#include "hub_crypt.h"
#include "nvs_flash.h"

static const char *TAG = "diag";
static const hub_state_t *s_st;

#define STA_GRACE_US   (120LL * 1000000)
#define ZB_GRACE_US    (90LL * 1000000)
#define LINK_GRACE_US  (300LL * 1000000)
#define MAX_DEV        8

static int64_t s_sta_down_since, s_link_down_since;
static hub_dev_t s_dev[MAX_DEV];   /* evaluate·dump 공용(같은 태스크) — 부팅 직후 힙이 빠듯해 하나만 */

static bool sta_connected(wifi_ap_record_t *ap)
{
    wifi_ap_record_t tmp;
    return esp_wifi_sta_get_ap_info(ap ? ap : &tmp) == ESP_OK;
}

static uint32_t evaluate(void)
{
    int64_t now = esp_timer_get_time();
    uint32_t m = 0;
    if (!s_st || !s_st->provisioned) {
        return 0;                   /* 첫 설정 전엔 끊김이 정상 */
    }
    if (sta_connected(NULL) || hub_wifi_radio_paused()) {
        s_sta_down_since = 0;
    } else if (!s_sta_down_since) {
        s_sta_down_since = now;
    } else if (now - s_sta_down_since > STA_GRACE_US) {
        m |= HUB_LED_BIT(HUB_LED_CODE_WIFI);
    }
    if (hub_zigbee_is_ready() && now > ZB_GRACE_US) {
        esp_zigbee_lock_acquire(portMAX_DELAY);
        bool joined = ezb_bdb_dev_joined();
        esp_zigbee_lock_release();
        if (!joined) {
            m |= HUB_LED_BIT(HUB_LED_CODE_ZIGBEE);
        }
    }
    hub_dev_t *dev = s_dev;
    int n = hub_rinnai_snapshot(dev, MAX_DEV);
    bool down = false;
    for (int i = 0; i < n; i++) {
        down |= !dev[i].linked;
    }
    if (!down) {
        s_link_down_since = 0;
    } else if (!s_link_down_since) {
        s_link_down_since = now;
    } else if (now - s_link_down_since > LINK_GRACE_US) {
        m |= HUB_LED_BIT(HUB_LED_CODE_RINNAI);
    }
    return m;
}

static const char *reset_text(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "전원 켬";
    case ESP_RST_SW:       return "소프트웨어 재시작";
    case ESP_RST_PANIC:    return "패닉(크래시)";
    case ESP_RST_INT_WDT:  return "인터럽트 워치독";
    case ESP_RST_TASK_WDT: return "태스크 워치독";
    case ESP_RST_WDT:      return "워치독";
    case ESP_RST_BROWNOUT: return "전압 부족";
    case ESP_RST_DEEPSLEEP: return "딥슬립 깸";
    case ESP_RST_EXT:      return "외부 리셋";
    default:               return "기타";
    }
}

static void dump(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    int64_t up = esp_timer_get_time() / 1000000;
    printf("\n#DIAG-BEGIN\n");
    printf("fw=%s built=%s %s idf=%s\n", app->version, app->date, app->time, app->idf_ver);
    printf("up=%llds reset=%d(%s) heap=%lu heap_min=%lu\n", (long long)up, esp_reset_reason(),
           reset_text(esp_reset_reason()), (unsigned long)esp_get_free_heap_size(),
           (unsigned long)esp_get_minimum_free_heap_size());

    uint32_t codes = hub_led_codes_get();
    printf("led_codes=0x%lx", (unsigned long)codes);
    for (int c = 1; c <= HUB_LED_CODE_MAX; c++) {
        if (codes & HUB_LED_BIT(c)) {
            printf(" [%d:%s]", c, hub_led_code_text(c));
        }
    }
    printf("\n");

    if (s_st) {
        printf("device_id=%s provisioned=%d sta_ssid=%s ap_ssid=%s (암호는 출력 안 함)\n", s_st->device_id,
               s_st->provisioned, s_st->sta_ssid, s_st->ap_ssid);
    }
    wifi_ap_record_t ap;
    if (sta_connected(&ap)) {
        esp_netif_ip_info_t ip = { 0 };
        esp_netif_t *nf = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (nf) {
            esp_netif_get_ip_info(nf, &ip);
        }
        printf("sta=up rssi=%d ch=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x ip=" IPSTR "\n", ap.rssi, ap.primary,
               ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5], IP2STR(&ip.ip));
    } else {
        printf("sta=down paused=%d down_for=%llds\n", hub_wifi_radio_paused(),
               s_sta_down_since ? (long long)((esp_timer_get_time() - s_sta_down_since) / 1000000) : 0LL);
    }
    wifi_sta_list_t sl;
    if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) {
        printf("ap_clients=%d", sl.num);
        for (int i = 0; i < sl.num; i++) {
            printf(" %02x:%02x:%02x:%02x:%02x:%02x(%d)", sl.sta[i].mac[0], sl.sta[i].mac[1], sl.sta[i].mac[2],
                   sl.sta[i].mac[3], sl.sta[i].mac[4], sl.sta[i].mac[5], sl.sta[i].rssi);
        }
        printf("\n");
    }

    if (hub_zigbee_is_ready()) {
        esp_zigbee_lock_acquire(portMAX_DELAY);
        bool joined = ezb_bdb_dev_joined();
        bool fresh = ezb_bdb_is_factory_new();
        uint16_t pan = ezb_nwk_get_panid();
        uint8_t ch = ezb_nwk_get_current_channel();
        uint16_t sa = ezb_nwk_get_short_address();
        esp_zigbee_lock_release();
        extern bool g_zb_nvs_was_reset;
        printf("zb joined=%d factory_new=%d nvs_reset=%d pan=0x%04x ch=%d short=0x%04x\n", joined, fresh,
               g_zb_nvs_was_reset, pan, ch, sa);
    } else {
        printf("zb ready=0\n");
    }
    printf("uplink=0x%02x mode=%s\n", hub_uplink_mask(), hub_proxy_mode() == HUB_MODE_STANDALONE ? "standalone" : "proxy");

    hub_dev_t *dev = s_dev;
    int n = hub_rinnai_snapshot(dev, MAX_DEV);
    for (int i = 0; i < n; i++) {
        printf("dev %s %02x%02x%02x linked=%d blob=%d\n", dev[i].kind == HUB_KIND_F06 ? "보일러조절기" : "온도조절기",
               dev[i].mac3[0], dev[i].mac3[1], dev[i].mac3[2], dev[i].linked, dev[i].n);
    }
    if (!n) {
        printf("dev (없음)\n");
    }

    nvs_stats_t ns;
    if (nvs_get_stats(NULL, &ns) == ESP_OK) {        /* 앱 nvs 사용량(항목 32바이트 단위) */
        printf("nvs used=%u free=%u total=%u ns=%u\n", (unsigned)ns.used_entries, (unsigned)ns.free_entries,
               (unsigned)ns.total_entries, (unsigned)ns.namespace_count);
    }
    hub_crypt_print();
    char errs[4 * 60 + 8];
    hub_store_errors_json(errs, sizeof(errs));
    printf("store_bad=%d errors=%s\n", hub_store_error_count(), errs);
    hub_events_print(40);
    printf("diag_stack_free=%u\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    printf("#DIAG-END\n");
    fflush(stdout);
}

static void help(void)
{
    printf("\n#LYNNAI 시리얼 명령: diag(상태 전체) / led(LED 코드 뜻) / mode [proxy|standalone](동작 모드) / crypt(암호화·eFuse 상태) / help\n");
    printf("#LED 빨강 N번 깜빡 → 2초 쉼: ");
    for (int c = 1; c <= HUB_LED_CODE_MAX; c++) {
        printf("%d=%s%s", c, hub_led_code_text(c), c < HUB_LED_CODE_MAX ? ", " : "\n");
    }
    fflush(stdout);
}

static void on_line(char *l)
{
    while (*l == ' ' || *l == '\r') {
        l++;
    }
    size_t n = strlen(l);
    while (n && (l[n - 1] == ' ' || l[n - 1] == '\r')) {
        l[--n] = 0;
    }
    if (!n) {
        return;
    }
    if (!strcmp(l, "diag")) {
        dump();
    } else if (!strcmp(l, "crypt")) {
        hub_crypt_print();
#if HUB_CRYPT_SELFTEST
    } else if (!strcmp(l, "crypt selftest")) {
        extern void hub_crypt_request_selftest(void);
        printf("#암호화 왕복 시험: 재시작 후 부팅 때 실행(eFuse 안 건드림)\n");
        hub_crypt_request_selftest();
#endif
    } else if (!strncmp(l, "mode", 4)) {
        /* USB(물리 접근) = 관리자와 같은 권한: 관리 화면에 못 들어갈 때 동작 모드를 바꾸거나 되돌린다 */
        const char *arg = l + 4;
        while (*arg == ' ') {
            arg++;
        }
        if (!*arg) {
            printf("#mode=%s\n", hub_proxy_mode() == HUB_MODE_STANDALONE ? "standalone" : "proxy");
        } else {
            char form[32], msg[160] = "";
            snprintf(form, sizeof(form), "mode=%s", arg);
            bool ok = hub_proxy_apply(form, msg, sizeof(msg));
            printf("#mode %s: %s\n", ok ? "ok" : "fail", msg);
        }
    } else if (!strcmp(l, "led") || !strcmp(l, "help")) {
        help();
    } else {
        printf("#알 수 없는 명령 '%s' — help\n", l);
    }
}

static void diag_task(void *arg)
{
    /* 콘솔 UART0 에 수신 드라이버만 붙인다(로그 출력은 그대로 VFS 경로) */
    bool rx = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0) == ESP_OK;
    if (!rx) {
        ESP_LOGW(TAG, "UART0 수신 드라이버 실패 — 시리얼 명령 없이 LED 코드만");
    }
    char line[32];
    int len = 0;
    int64_t next_eval = 0;
    for (;;) {
        uint8_t c;
        int r = rx ? uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(500)) : (vTaskDelay(pdMS_TO_TICKS(500)), 0);
        if (r == 1) {
            if (c == '\n') {
                line[len] = 0;
                on_line(line);
                len = 0;
            } else if (len < (int)sizeof(line) - 1) {
                line[len++] = (char)c;
            }
        }
        int64_t now = esp_timer_get_time();
        if (now >= next_eval) {
            next_eval = now + 5 * 1000000;
            uint32_t prev = hub_led_codes_get();
            hub_led_codes(evaluate());
            uint32_t cur = hub_led_codes_get();
            if (cur != prev) {
                ESP_LOGW(TAG, "LED 오류 코드 0x%lx → 0x%lx (시리얼에 diag 입력하면 자세히)", (unsigned long)prev,
                         (unsigned long)cur);
            }
        }
    }
}

void hub_diag_start(const hub_state_t *st)
{
    s_st = st;
    xTaskCreate(diag_task, "diag", 3584, NULL, 2, NULL);   /* diag 출력 시 여유 ~1.1KB(실측) */
}
