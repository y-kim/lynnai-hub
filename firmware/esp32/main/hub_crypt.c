/*
 * hub_crypt.c -- 저장 데이터(NVS) 암호화. 설명은 hub_crypt.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_http_server.h"
#include "esp_efuse.h"
#include "esp_efuse_chip.h"
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
#include "bootloader_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "nvs_sec_provider.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hub_crypt.h"

static const char *TAG = "HUB_CRYPT";

#define PART_MAIN   "nvs"
#define PART_ZB     "zb_nvs"
#define NS_CRYPT    "crypt"         /* 평문 nvs 에만 있는 요청 표시(옮길 때 버린다) */
#define MIGRATE_MAX (48 * 1024)     /* RAM 으로 옮길 수 있는 최대(넘으면 굽기 전에 그만) */

static bool s_on;
static int s_key = -1;

bool hub_crypt_on(void) { return s_on; }
int hub_crypt_key_block(void) { return s_key; }

/* ---------------------------------------------------------------- eFuse */

static int find_hmac_key(void)
{
    esp_efuse_block_t b;
    return esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &b) ? (int)(b - EFUSE_BLK_KEY0) : -1;
}

static const char *purpose_name(esp_efuse_purpose_t p)
{
    switch (p) {
    case ESP_EFUSE_KEY_PURPOSE_USER: return "사용자 데이터";
    case ESP_EFUSE_KEY_PURPOSE_XTS_AES_128_KEY: return "플래시 암호화(XTS-AES)";
    case ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_ALL: return "HMAC(하향, 전체)";
    case ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_JTAG: return "JTAG 잠금 해제";
    case ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_DIGITAL_SIGNATURE: return "디지털 서명";
    case ESP_EFUSE_KEY_PURPOSE_HMAC_UP: return "HMAC(상향) — 저장 데이터 암호화";
    case ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST0:
    case ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST1:
    case ESP_EFUSE_KEY_PURPOSE_SECURE_BOOT_DIGEST2: return "보안 부팅 키 요약";
    default: return "기타";
    }
}

/* 고를 수 있는 블록: 비어 있거나(굽는다) 이미 HMAC_UP 키가 있는(그대로 쓴다) 블록 */
static bool selectable(int k)
{
    esp_efuse_block_t b = EFUSE_BLK_KEY0 + k;
    return esp_efuse_key_block_unused(b) || esp_efuse_get_key_purpose(b) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP;
}

static esp_err_t sec_cfg_hmac(int key, nvs_sec_cfg_t *cfg)
{
    static nvs_sec_scheme_t *sch;
    if (!sch) {
        nvs_sec_config_hmac_t c = { .hmac_key_id = (hmac_key_id_t)key };
        esp_err_t e = nvs_sec_provider_register_hmac(&c, &sch);
        if (e != ESP_OK) {
            return e;
        }
    }
    return nvs_flash_read_security_cfg_v2(sch, cfg);   /* 키가 없으면 실패(굽지 않는다) */
}

/* ---------------------------------------------------------------- 파티션 열기 */

static esp_err_t open_part(const char *name, nvs_sec_cfg_t *cfg, bool *reset)
{
    esp_err_t e = cfg ? nvs_flash_secure_init_partition(name, cfg) : nvs_flash_init_partition(name);
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "%s 초기화 실패(%s) → 지우고 새로 만듦", name, esp_err_to_name(e));
        if (reset) {
            *reset = true;
        }
        esp_err_t er = nvs_flash_erase_partition(name);
        if (er != ESP_OK) {
            return er;
        }
        e = cfg ? nvs_flash_secure_init_partition(name, cfg) : nvs_flash_init_partition(name);
    }
    return e;
}

/* ---------------------------------------------------------------- 옮기기(RAM 으로 읽고 다시 쓰기) */

typedef struct ent {
    struct ent *next;
    char ns[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    size_t len;
    uint8_t data[];
} ent_t;

static void free_list(ent_t *l)
{
    while (l) {
        ent_t *n = l->next;
        free(l);
        l = n;
    }
}

static size_t int_len(nvs_type_t t)
{
    switch (t) {
    case NVS_TYPE_U8: case NVS_TYPE_I8: return 1;
    case NVS_TYPE_U16: case NVS_TYPE_I16: return 2;
    case NVS_TYPE_U32: case NVS_TYPE_I32: return 4;
    case NVS_TYPE_U64: case NVS_TYPE_I64: return 8;
    default: return 0;
    }
}

static esp_err_t get_int(nvs_handle_t h, const char *k, nvs_type_t t, void *out)
{
    switch (t) {
    case NVS_TYPE_U8: return nvs_get_u8(h, k, out);
    case NVS_TYPE_I8: return nvs_get_i8(h, k, out);
    case NVS_TYPE_U16: return nvs_get_u16(h, k, out);
    case NVS_TYPE_I16: return nvs_get_i16(h, k, out);
    case NVS_TYPE_U32: return nvs_get_u32(h, k, out);
    case NVS_TYPE_I32: return nvs_get_i32(h, k, out);
    case NVS_TYPE_U64: return nvs_get_u64(h, k, out);
    case NVS_TYPE_I64: return nvs_get_i64(h, k, out);
    default: return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t set_any(nvs_handle_t h, const ent_t *e)
{
    const void *d = e->data;
    switch (e->type) {
    case NVS_TYPE_U8: return nvs_set_u8(h, e->key, *(const uint8_t *)d);
    case NVS_TYPE_I8: return nvs_set_i8(h, e->key, *(const int8_t *)d);
    case NVS_TYPE_U16: return nvs_set_u16(h, e->key, *(const uint16_t *)d);
    case NVS_TYPE_I16: return nvs_set_i16(h, e->key, *(const int16_t *)d);
    case NVS_TYPE_U32: return nvs_set_u32(h, e->key, *(const uint32_t *)d);
    case NVS_TYPE_I32: return nvs_set_i32(h, e->key, *(const int32_t *)d);
    case NVS_TYPE_U64: return nvs_set_u64(h, e->key, *(const uint64_t *)d);
    case NVS_TYPE_I64: return nvs_set_i64(h, e->key, *(const int64_t *)d);
    case NVS_TYPE_STR: return nvs_set_str(h, e->key, (const char *)d);
    case NVS_TYPE_BLOB: return nvs_set_blob(h, e->key, d, e->len);
    default: return ESP_ERR_INVALID_ARG;
    }
}

/* 파티션의 모든 항목을 RAM 목록으로(skip_ns 네임스페이스는 뺀다). *total 에 누적 크기 */
static esp_err_t collect(const char *part, const char *skip_ns, ent_t **out, int *count, size_t *total)
{
    nvs_iterator_t it = NULL;
    ent_t *head = NULL, **tail = &head;
    *count = 0;
    esp_err_t r = nvs_entry_find(part, NULL, NVS_TYPE_ANY, &it);
    esp_err_t err = ESP_OK;
    while (r == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (!(skip_ns && !strcmp(info.namespace_name, skip_ns))) {
            nvs_handle_t h;
            err = nvs_open_from_partition(part, info.namespace_name, NVS_READONLY, &h);
            if (err != ESP_OK) {
                break;
            }
            size_t len = int_len(info.type);
            if (info.type == NVS_TYPE_STR) {
                err = nvs_get_str(h, info.key, NULL, &len);
            } else if (info.type == NVS_TYPE_BLOB) {
                err = nvs_get_blob(h, info.key, NULL, &len);
            } else if (!len) {
                err = ESP_ERR_NOT_SUPPORTED;
            }
            ent_t *e = err == ESP_OK ? malloc(sizeof(ent_t) + len + 1) : NULL;
            if (err == ESP_OK && !e) {
                err = ESP_ERR_NO_MEM;
            }
            if (err == ESP_OK) {
                *total += sizeof(ent_t) + len;
                if (*total > MIGRATE_MAX) {
                    err = ESP_ERR_NO_MEM;
                    free(e);
                } else {
                    memset(e, 0, sizeof(ent_t));
                    strlcpy(e->ns, info.namespace_name, sizeof(e->ns));
                    strlcpy(e->key, info.key, sizeof(e->key));
                    e->type = info.type;
                    e->len = len;
                    size_t l2 = len;
                    err = info.type == NVS_TYPE_STR ? nvs_get_str(h, info.key, (char *)e->data, &l2)
                        : info.type == NVS_TYPE_BLOB ? nvs_get_blob(h, info.key, e->data, &l2)
                        : get_int(h, info.key, info.type, e->data);
                    if (err == ESP_OK) {
                        *tail = e;
                        tail = &e->next;
                        (*count)++;
                    } else {
                        free(e);
                    }
                }
            }
            nvs_close(h);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "%s/%s:%s 읽기 실패(%s)", part, info.namespace_name, info.key, esp_err_to_name(err));
                break;
            }
        }
        r = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (err != ESP_OK) {
        free_list(head);
        return err;
    }
    *out = head;
    return ESP_OK;
}

static esp_err_t restore(const char *part, const ent_t *l)
{
    for (; l; l = l->next) {
        nvs_handle_t h;
        esp_err_t e = nvs_open_from_partition(part, l->ns, NVS_READWRITE, &h);
        if (e == ESP_OK) {
            e = set_any(h, l);
            if (e == ESP_OK) {
                e = nvs_commit(h);
            }
            nvs_close(h);
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "%s/%s:%s 쓰기 실패(%s)", part, l->ns, l->key, esp_err_to_name(e));
            return e;
        }
    }
    return ESP_OK;
}

/* 파티션을 지우고 cfg(NULL=평문)로 다시 열어 목록을 되쓴다 */
static esp_err_t rewrite(const char *part, nvs_sec_cfg_t *cfg, const ent_t *l)
{
    nvs_flash_deinit_partition(part);
    esp_err_t e = nvs_flash_erase_partition(part);
    if (e == ESP_OK) {
        e = cfg ? nvs_flash_secure_init_partition(part, cfg) : nvs_flash_init_partition(part);
    }
    return e == ESP_OK ? restore(part, l) : e;
}

/* 두 파티션을 평문으로 열어 다 읽는다 */
static esp_err_t collect_both(ent_t **a, ent_t **b, size_t *total)
{
    int na = 0, nb = 0;
    *total = 0;
    esp_err_t e = open_part(PART_ZB, NULL, NULL);
    if (e == ESP_OK) {
        e = collect(PART_MAIN, NS_CRYPT, a, &na, total);
    }
    if (e == ESP_OK) {
        e = collect(PART_ZB, NULL, b, &nb, total);
        if (e != ESP_OK) {
            free_list(*a);
        }
    }
    if (e == ESP_OK) {
        ESP_LOGW(TAG, "옮길 데이터: nvs %d개 + zb_nvs %d개, %u바이트", na, nb, (unsigned)*total);
    }
    return e;
}

static void clear_request(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(PART_MAIN, NS_CRYPT, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 암호화 켜기(부팅 때): 굽기 전에 다 읽어 두고, 굽고, 지우고, 암호화로 다시 쓴다. true = 끝까지 됨 */
static bool migrate(int key)
{
    ent_t *a = NULL, *b = NULL;
    size_t total;
    if (collect_both(&a, &b, &total) != ESP_OK) {
        ESP_LOGE(TAG, "데이터를 다 읽지 못함(%u바이트 한도) — 굽지 않고 그만", (unsigned)MIGRATE_MAX);
        return false;
    }
    esp_efuse_block_t blk = EFUSE_BLK_KEY0 + key;
    bool ok = false;
    if (esp_efuse_key_block_unused(blk)) {
        uint8_t k[32];
        bootloader_random_enable();                /* Wi-Fi 전이라 하드웨어 난수원을 켜고 */
        esp_fill_random(k, sizeof(k));
        bootloader_random_disable();
        esp_err_t e = esp_efuse_write_key(blk, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, k, sizeof(k));
        memset(k, 0, sizeof(k));
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "eFuse KEY%d 굽기 실패(%s) — 데이터는 그대로", key, esp_err_to_name(e));
            goto out;
        }
        ESP_LOGW(TAG, "eFuse KEY%d 에 HMAC 키를 구움(읽기·쓰기 금지됨)", key);
    } else if (esp_efuse_get_key_purpose(blk) != ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
        ESP_LOGE(TAG, "KEY%d 는 다른 용도로 쓰는 중 — 그만", key);
        goto out;
    }
    nvs_sec_cfg_t cfg;
    if (sec_cfg_hmac(key, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HMAC 로 암호화 키를 못 만듦 — 데이터는 그대로");
        goto out;
    }
    /* 여기부터 되돌릴 수 없음: 지우고 암호화로 다시 쓴다(중간에 전원이 끊기면 설정·가입을 다시 해야 한다) */
    if (rewrite(PART_MAIN, &cfg, a) != ESP_OK || rewrite(PART_ZB, &cfg, b) != ESP_OK) {
        ESP_LOGE(TAG, "암호화로 되쓰기 실패");
        goto out;
    }
    ok = true;
out:
    free_list(a);
    free_list(b);
    return ok;
}

#if HUB_CRYPT_SELFTEST
/* 시험: 고정 키로 암호화 → 다시 읽어 비교 → 평문으로 되돌림 → 비교. eFuse 는 건드리지 않는다. */
static bool same(const char *part, const ent_t *l)
{
    int n = 0;
    size_t tot = 0;
    ent_t *r = NULL;
    if (collect(part, NULL, &r, &n, &tot) != ESP_OK) {
        return false;
    }
    bool ok = true;
    int m = 0;
    for (const ent_t *x = l; x; x = x->next, m++) {
        const ent_t *y = r;
        while (y && (strcmp(x->ns, y->ns) || strcmp(x->key, y->key))) {
            y = y->next;
        }
        if (!y || y->type != x->type || y->len != x->len || memcmp(y->data, x->data, x->len)) {
            ESP_LOGE(TAG, "시험: %s/%s:%s 다름", part, x->ns, x->key);
            ok = false;
        }
    }
    free_list(r);
    ESP_LOGW(TAG, "시험: %s 항목 %d개 비교 %s(읽은 것 %d개)", part, m, ok ? "같음" : "다름", n);
    return ok && n == m;
}

static void selftest(void)
{
    ent_t *a = NULL, *b = NULL;
    size_t total;
    clear_request();
    if (collect_both(&a, &b, &total) != ESP_OK) {
        ESP_LOGE(TAG, "시험: 읽기 실패");
        return;
    }
    nvs_sec_cfg_t cfg;
    memset(cfg.eky, 0x5a, sizeof(cfg.eky));        /* 시험용 고정 키(실제는 eFuse HMAC 에서) */
    memset(cfg.tky, 0xa5, sizeof(cfg.tky));
    bool ok = rewrite(PART_MAIN, &cfg, a) == ESP_OK && rewrite(PART_ZB, &cfg, b) == ESP_OK;
    ESP_LOGW(TAG, "시험 1/2: 암호화로 되쓰기 %s", ok ? "성공" : "실패");
    ok = ok && same(PART_MAIN, a) && same(PART_ZB, b);
    bool back = rewrite(PART_MAIN, NULL, a) == ESP_OK && rewrite(PART_ZB, NULL, b) == ESP_OK;
    back = back && same(PART_MAIN, a) && same(PART_ZB, b);
    ESP_LOGW(TAG, "시험 결과: 암호화 왕복 %s, 평문 복원 %s", ok ? "통과" : "실패", back ? "통과" : "실패");
    free_list(a);
    free_list(b);
}
#endif

/* ---------------------------------------------------------------- 부팅 */

esp_err_t hub_crypt_boot(bool *zb_reset)
{
    int key = find_hmac_key();
    if (key >= 0) {
        nvs_sec_cfg_t cfg;
        esp_err_t e = sec_cfg_hmac(key, &cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "eFuse KEY%d HMAC 로 저장소 키를 못 만듦(%s)", key, esp_err_to_name(e));
            return e;
        }
        e = open_part(PART_MAIN, &cfg, NULL);
        if (e == ESP_OK) {
            e = open_part(PART_ZB, &cfg, zb_reset);
        }
        if (e == ESP_OK) {
            s_on = true;
            s_key = key;
            ESP_LOGI(TAG, "저장 데이터 암호화 켜짐(eFuse KEY%d, HMAC)", key);
        }
        return e;
    }
    esp_err_t e = open_part(PART_MAIN, NULL, NULL);
    if (e != ESP_OK) {
        return e;
    }
    nvs_handle_t h;
    int32_t req = -1, test = 0;
    if (nvs_open_from_partition(PART_MAIN, NS_CRYPT, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "pending", &req);
        nvs_get_i32(h, "selftest", &test);
        nvs_close(h);
    }
#if HUB_CRYPT_SELFTEST
    if (test == 1) {
        selftest();
    }
#endif
    if (req >= 0 && req <= 5) {
        ESP_LOGW(TAG, "암호화 켜기 요청 처리: eFuse KEY%d", (int)req);
        if (migrate((int)req)) {
            ESP_LOGW(TAG, "저장 데이터를 암호화로 옮김 — 재시작");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
        clear_request();                           /* 굽기 전에 실패: 평문 그대로, 요청은 지움(반복 안 하게) */
    }
    return open_part(PART_ZB, NULL, zb_reset);
}

/* ---------------------------------------------------------------- 관리 화면 */

void hub_crypt_render(char *o, size_t cap)
{
    char t[384];
    strlcat(o, "<h2>저장 데이터 암호화</h2><div class=card>", cap);
    if (s_on) {
        snprintf(t, sizeof(t), "<p class=ok>켜짐 — eFuse KEY%d 의 HMAC 키로 Wi-Fi 암호·API 토큰·Zigbee 가입 정보 등 "
                 "저장 데이터 전체를 암호화합니다. 키는 칩 밖으로 읽을 수 없습니다.</p>", s_key);
    } else {
        snprintf(t, sizeof(t), "<p class=warn>꺼짐 — 저장 데이터가 평문입니다(USB 로 플래시를 읽으면 Wi-Fi 암호 등이 보임).</p>");
    }
    strlcat(o, t, cap);
    snprintf(t, sizeof(t), "<p class=muted>플래시 암호화 %s · 보안 부팅 %s</p>", esp_flash_encryption_enabled() ? "켜짐" : "꺼짐",
             esp_secure_boot_enabled() ? "켜짐" : "꺼짐");
    strlcat(o, t, cap);
    if (!s_on) {
        strlcat(o, "<form method=post action=/admin/crypt onsubmit=\"return confirm('고른 eFuse 블록에 키를 굽고 저장 데이터를 "
                   "암호화합니다. eFuse 는 되돌릴 수 없고 암호화는 끌 수 없습니다. 계속할까요?')\">"
                   "<p class=muted>키를 넣을 eFuse 블록을 고르세요. 비어 있으면 새 키를 굽고, 이미 HMAC 키가 있으면 그대로 씁니다.</p>",
                cap);
    }
    for (int k = 0; k < 6; k++) {
        esp_efuse_block_t b = EFUSE_BLK_KEY0 + k;
        bool unused = esp_efuse_key_block_unused(b);
        const char *st = unused ? "비어 있음 — 여기에 새 키를 굽습니다" : purpose_name(esp_efuse_get_key_purpose(b));
        if (s_on) {
            snprintf(t, sizeof(t), "<div class=row><span>KEY%d%s</span><span class=muted>%s</span></div>", k,
                     k == s_key ? " (사용 중)" : "", unused ? "비어 있음" : st);
        } else {
            bool sel = selectable(k);
            snprintf(t, sizeof(t), "<div class=row><label class=chk><input type=radio name=key value=%d%s> KEY%d</label>"
                     "<span class=muted>%s</span></div>", k, sel ? "" : " disabled", k, st);
        }
        strlcat(o, t, cap);
    }
    if (!s_on) {
        strlcat(o, "<label class=chk><input type=checkbox name=ok> eFuse 는 한 번 쓰면 되돌릴 수 없고, 암호화를 켜면 끌 수 "
                   "없다는 것을 이해했습니다</label>"
                   "<p class=muted>저장 후 허브가 재시작하며 옮깁니다(수 초). 그동안 전원을 끄지 마세요 — 끊기면 설정과 "
                   "Zigbee 가입을 다시 해야 합니다.</p><button class=sv>암호화 켜기 (재시작)</button></form>", cap);
    }
    strlcat(o, "</div>", cap);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));               /* 응답을 먼저 보내고 */
    esp_restart();
}

bool hub_crypt_apply(const char *form, char *msg, size_t msglen)
{
    char v[8];
    if (s_on) {
        snprintf(msg, msglen, "이미 암호화되어 있습니다");
        return false;
    }
    if (httpd_query_key_value(form, "ok", v, sizeof(v)) != ESP_OK) {
        snprintf(msg, msglen, "되돌릴 수 없다는 확인에 체크해 주세요");
        return false;
    }
    if (httpd_query_key_value(form, "key", v, sizeof(v)) != ESP_OK || v[0] < '0' || v[0] > '5' || v[1]) {
        snprintf(msg, msglen, "eFuse 블록을 고르세요");
        return false;
    }
    int k = v[0] - '0';
    if (!selectable(k)) {
        snprintf(msg, msglen, "KEY%d 는 다른 용도로 쓰는 중이라 고를 수 없습니다", k);
        return false;
    }
    nvs_handle_t h;
    esp_err_t e = nvs_open_from_partition(PART_MAIN, NS_CRYPT, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_i32(h, "pending", k);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e != ESP_OK) {
        snprintf(msg, msglen, "요청을 저장하지 못했습니다(%s)", esp_err_to_name(e));
        return false;
    }
    ESP_LOGW(TAG, "암호화 켜기 요청(KEY%d) — 재시작", k);
    xTaskCreate(restart_task, "crypt_rst", 2048, NULL, 5, NULL);
    snprintf(msg, msglen, "허브가 재시작하며 KEY%d 로 암호화합니다(수 초). 잠시 뒤 다시 접속하세요", k);
    return true;
}

void hub_crypt_print(void)
{
    printf("crypt=%s key=%d flash_enc=%d secure_boot=%d", s_on ? "on" : "off", s_key, esp_flash_encryption_enabled(),
           esp_secure_boot_enabled());
    for (int k = 0; k < 6; k++) {
        esp_efuse_block_t b = EFUSE_BLK_KEY0 + k;
        printf(" KEY%d=%s", k, esp_efuse_key_block_unused(b) ? "empty" : esp_efuse_get_key_purpose(b) ==
               ESP_EFUSE_KEY_PURPOSE_HMAC_UP ? "hmac_up" : "used");
    }
    printf("\n");
#if HUB_CRYPT_SELFTEST
    printf("#crypt selftest 가능(시험 빌드)\n");
#endif
}

#if HUB_CRYPT_SELFTEST
void hub_crypt_request_selftest(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(PART_MAIN, NS_CRYPT, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "selftest", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    xTaskCreate(restart_task, "crypt_rst", 2048, NULL, 5, NULL);
}
#endif
