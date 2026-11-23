/*
 * hub_ota.c -- WiFi 펌웨어 갱신(OTA) + 원격 재부팅. 설명은 hub_ota.h.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs.h"
#include "esp_wifi.h"
#include "esp_timer.h"

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "hub_ota.h"
#include "hub_bbox.h"
#include "hub_sensors.h"
#include "hub_diag.h"

static const char *TAG = "HUB_OTA";

/* 이미지 표시: 이미지의 고정된 자리(앱 설명 바로 뒤)에 넣어, 올라온 파일이 LYNNAI 허브 펌웨어인지 확인한다.
 * kind 는 늘 0 이다. v0.5 까지는 Zigbee 라디오를 UART 로 따로 붙이는 펌웨어(kind 1)가 따로 있었고, 그때의 펌웨어는
 * 자기와 kind 가 같은 이미지만 적용하므로 이 자리와 값은 바꾸지 않는다(바꾸면 v0.5 에서 OTA 로 올라오지 못한다). */
#define IMG_MAGIC   0x524E594Cu        /* "LYNR" */
#define IMG_KIND    0
typedef struct {
    uint32_t magic;
    uint8_t kind;
    uint8_t rsv[3];
} hub_img_desc_t;
const __attribute__((section(".rodata_custom_desc"), used)) hub_img_desc_t hub_img_desc = { .magic = IMG_MAGIC, .kind = IMG_KIND };
#define IMG_DESC_OFF (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

/* 한 칸에 담긴 펌웨어의 종류: 0 = 이 펌웨어와 같은 종류, 1 = 옛 외부 라디오용(쓸 수 없음), -1 = 표시 없음(비었거나 남의 것) */
static int image_kind(const esp_partition_t *p)
{
    hub_img_desc_t d;
    if (!p || esp_partition_read(p, IMG_DESC_OFF, &d, sizeof(d)) != ESP_OK || d.magic != IMG_MAGIC) {
        return -1;
    }
    return d.kind;
}

#define OTA_RX_CHUNK 4096

/* ---------------------------------------------------------------- 자가 되돌리기(설명은 hub_ota.h) */
#define TRIAL_MAX      3        /* 확정 못 한 부팅을 이만큼 허용 */
#define TRIAL_OK_S     45       /* 이만큼 떠 있고 집 Wi-Fi 에 붙어 있으면 확정 */
#define TRIAL_GIVEUP_S 300      /* 이때까지 확정 못 하면 재시작(실패 1회) */

static uint8_t s_trial;         /* 이번 부팅이 몇 번째 시험 부팅인가(0 = 시험 아님) */
static bool s_rolled_back;      /* 지난 OTA 가 되돌려졌나(관리 화면 안내용) */

static uint8_t ota_get(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("ota", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void ota_set(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, key, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void trial_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        int64_t up = esp_timer_get_time() / 1000000;
        wifi_ap_record_t ap;
        if (up >= TRIAL_OK_S && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ota_set("trial", 0);
            ota_set("rb", 0);
            s_trial = 0;
            s_rolled_back = false;
            ESP_LOGW(TAG, "새 펌웨어 확정(부팅 %llds, 집 Wi-Fi 연결됨)", (long long)up);
            vTaskDelete(NULL);
        }
        if (up >= TRIAL_GIVEUP_S) {
            ESP_LOGE(TAG, "새 펌웨어가 %d초 안에 집 Wi-Fi 에 붙지 못함 — 재시작(시험 %d/%d)", TRIAL_GIVEUP_S, s_trial, TRIAL_MAX);
            esp_restart();
        }
    }
}

void hub_ota_boot_guard(void)
{
    uint8_t n = ota_get("trial");
    s_rolled_back = ota_get("rb") != 0;
    if (!n) {
        return;
    }
    if (n > TRIAL_MAX) {
        /* 새 펌웨어가 TRIAL_MAX 번 떠도 확정하지 못했다 — 이전 칸으로 */
        const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
        ota_set("trial", 0);
        if (other && image_kind(other) == IMG_KIND &&
            esp_ota_set_boot_partition(other) == ESP_OK) {
            ota_set("rb", 1);
            ESP_LOGE(TAG, "새 펌웨어가 확정되지 못함 — 이전 펌웨어(%s)로 되돌리고 재시작", other->label);
            esp_restart();
        }
        ESP_LOGE(TAG, "되돌릴 이전 펌웨어가 없음 — 이 펌웨어로 계속");
        return;
    }
    s_trial = n;
    ota_set("trial", (uint8_t)(n + 1));
    ESP_LOGW(TAG, "새 펌웨어 시험 부팅 %d/%d — 집 Wi-Fi 에 붙으면 확정", n, TRIAL_MAX);
    xTaskCreate(trial_task, "ota_trial", 4096, NULL, 2, NULL);
}

void hub_ota_planned_reboot(void)
{
    hub_bbox_before_restart();          /* 부팅 기록: 재시작 직전의 Zigbee 프레임 카운터를 남긴다 */
    hub_sensors_flush();                /* 가스 사용량(지난 하루·한 시간)이 재시작을 넘겨 이어지게 */
    if (s_trial) {
        ota_set("trial", s_trial);      /* 이번 부팅은 세지 않는다(Zigbee 재가입 재시도 같은 의도한 재시작) */
    }
}

void hub_ota_state(int *trial, bool *rolled_back)
{
    if (trial) *trial = s_trial;
    if (rolled_back) *rolled_back = s_rolled_back;
}

int hub_ota_write_req(httpd_req_t *req, char *msg, size_t msglen, bool *do_reboot)
{
    *do_reboot = false;
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *tgt = esp_ota_get_next_update_partition(NULL);
    if (!tgt || tgt == run) {
        snprintf(msg, msglen, "쓸 수 있는 반대 칸이 없습니다");
        return 0;
    }
    int total = req->content_len;
    if (total <= 0) {
        snprintf(msg, msglen, "빈 업로드입니다");
        return 0;
    }
    if ((size_t)total > tgt->size) {
        snprintf(msg, msglen, "펌웨어가 칸보다 큽니다(%d > %u)", total, (unsigned)tgt->size);
        return 0;
    }
    esp_ota_handle_t h = 0;
    esp_err_t e = esp_ota_begin(tgt, OTA_SIZE_UNKNOWN, &h);   /* 칸을 지운다(수 초) */
    if (e != ESP_OK) {
        snprintf(msg, msglen, "OTA 시작 실패: %s", esp_err_to_name(e));
        return 0;
    }
    char *buf = malloc(OTA_RX_CHUNK);
    if (!buf) {
        esp_ota_abort(h);
        snprintf(msg, msglen, "메모리 부족");
        return 0;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf, OTA_RX_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            esp_ota_abort(h);
            snprintf(msg, msglen, "수신이 끊겼습니다(%d/%d바이트)", got, total);
            return 0;
        }
        e = esp_ota_write(h, buf, r);
        if (e != ESP_OK) {
            free(buf);
            esp_ota_abort(h);
            snprintf(msg, msglen, "쓰기 실패: %s", esp_err_to_name(e));
            return 0;
        }
        got += r;
    }
    free(buf);
    e = esp_ota_end(h);          /* 체크섬·(켜져 있으면)서명 검증 */
    if (e != ESP_OK) {
        snprintf(msg, msglen, "이미지 검증 실패: %s", esp_err_to_name(e));
        return 0;
    }
    int up = image_kind(tgt);
    if (up < 0) {
        snprintf(msg, msglen, "LYNNAI 허브 펌웨어가 아닙니다(표시 없음) — 적용하지 않음");
        return 0;
    }
    if (up != IMG_KIND) {
        snprintf(msg, msglen, "외부 라디오용 펌웨어(-app-rcp.bin)는 더 지원하지 않습니다 — 적용하지 않음. -app.bin 을 올리세요.");
        return 0;
    }
    e = esp_ota_set_boot_partition(tgt);
    if (e != ESP_OK) {
        snprintf(msg, msglen, "부팅 칸 전환 실패: %s", esp_err_to_name(e));
        return 0;
    }
    ota_set("rb", 0);
    ota_set("trial", 1);            /* 새 펌웨어는 시험 부팅으로 시작 — 확정 못 하면 스스로 이 칸으로 돌아온다 */
    ESP_LOGW(TAG, "OTA 적용: %s %d바이트 → 재시작", tgt->label, got);
    snprintf(msg, msglen, "완료: %d바이트. 새 펌웨어로 재시작합니다 — 잠시 뒤 관리 화면을 새로고침하세요.", got);
    *do_reboot = true;
    return 1;
}

static void reboot_task(void *a)
{
    (void)a;
    vTaskDelay(pdMS_TO_TICKS(1500));    /* 응답이 나갈 시간 */
    hub_ota_planned_reboot();
    ESP_LOGW(TAG, "재부팅");
    esp_restart();
}

void hub_ota_schedule_reboot(void)
{
    xTaskCreate(reboot_task, "reboot", 2560, NULL, 5, NULL);
}

void hub_ota_render(char *out, size_t cap)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *tgt = esp_ota_get_next_update_partition(NULL);
    char ln[360];

    strlcat(out, "<h2>시스템 · 펌웨어(OTA)</h2><div class=card>", cap);
    if (s_rolled_back) {
        strlcat(out, "<p class=warn>지난번에 올린 펌웨어는 정상 동작이 확인되지 않아 <b>이전 버전으로 되돌렸습니다</b>.</p>", cap);
    }
    if (s_trial) {
        strlcat(out, "<p class=muted>새 펌웨어를 시험하는 중입니다 — 집 Wi-Fi 에 붙어 45초가 지나면 확정됩니다.</p>", cap);
    }

    const esp_app_desc_t *ad = esp_app_get_description();
    esp_app_desc_t od;
    char prev[48] = "없음";             /* 반대 칸 = 직전 펌웨어(새 펌웨어가 확정되지 못하면 돌아갈 곳) */
    if (tgt && tgt != run && image_kind(tgt) == IMG_KIND && esp_ota_get_partition_description(tgt, &od) == ESP_OK) {
        snprintf(prev, sizeof(prev), "%.32s", od.version);
    }
    snprintf(ln, sizeof(ln), "<p class=muted>지금 펌웨어: <b>%s</b> (빌드 %s %s) · 칸 %s · 이전 펌웨어: %s</p>",
             ad->version, ad->date, ad->time, run ? run->label : "?", prev);
    strlcat(out, ln, cap);

    /* Zigbee 상태 한 줄 — 관리 화면에서도 보이게(선 없이 진단) */
    hub_zb_diag_t z;
    hub_diag_zb_get(&z);
    if (!z.ready) {
        strlcat(out, "<p class=muted>Zigbee: 아직 준비 안 됨</p>", cap);
    } else {
        char rssi[24] = "";
        if (z.parent_rssi != 127) {
            snprintf(rssi, sizeof(rssi), " · 부모신호 %ddBm", z.parent_rssi);
        }
        snprintf(ln, sizeof(ln),
                 "<p class=%s>Zigbee: <b>%s</b> · 채널 %u · PAN 0x%04x · 이웃 %d%s</p>",
                 z.joined ? "ok" : "warn", z.joined ? "가입됨" : "미가입(오프라인)",
                 z.channel, z.pan, z.nbr_count, rssi);
        strlcat(out, ln, cap);
    }

    strlcat(out,
            "<div class=row><input type=file id=otaf accept=\".bin\">"
            "<button type=button onclick=\"otaUp()\">펌웨어 올리기</button></div>"
            "<progress id=otap value=0 max=100 style=\"display:none;width:100%\"></progress>"
            "<p id=otam class=muted></p>", cap);

    strlcat(out,
            "<form method=post action=/admin/system "
            "onsubmit=\"return confirm('허브를 재부팅할까요? Zigbee 가 다시 가입됩니다.')\" style=\"margin-top:8px\">"
            "<button name=op value=reboot class=ghost>재부팅 (Zigbee 재가입)</button></form>", cap);

    strlcat(out,
            "<p class=muted>.bin 은 릴리즈의 <code>-app.bin</code>(직접 빌드했다면 <code>build/lynnai_hub.bin</code>). "
            "올리면 새 펌웨어로 재시작하고, 이전 펌웨어는 되돌림용으로 남습니다.</p>", cap);

    strlcat(out,
            "<script>function otaUp(){var f=document.getElementById('otaf').files[0];"
            "var m=document.getElementById('otam'),p=document.getElementById('otap');"
            "if(!f){m.className='warn';m.textContent='파일을 고르세요';return;}"
            "m.className='muted';m.textContent='올리는 중…';p.style.display='block';p.value=0;"
            "var x=new XMLHttpRequest();x.open('POST','/admin/ota');"
            "x.upload.onprogress=function(e){if(e.lengthComputable)p.value=Math.round(e.loaded/e.total*100);};"
            "x.onload=function(){p.style.display='none';m.textContent=x.responseText;m.className=x.status==200?'ok':'warn';};"
            "x.onerror=function(){p.style.display='none';m.className='warn';m.textContent='전송 오류';};"
            "x.send(f);}</script></div>", cap);
}
