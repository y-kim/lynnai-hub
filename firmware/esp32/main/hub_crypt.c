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

#include <stddef.h>
#include "esp_partition.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "hub_crypt.h"
#include "hub_led.h"

static const char *TAG = "HUB_CRYPT";

#define PART_MAIN   "nvs"
#define PART_ZB     "zb_nvs"
#define NS_CRYPT    "crypt"         /* 평문 nvs 에만 있는 요청 표시(옮길 때 버린다) */
#define MIGRATE_MAX (48 * 1024)     /* RAM 으로 옮길 수 있는 최대(넘으면 굽기 전에 그만) */

static bool s_on;
static int s_key = -1;
static int s_arm_key;                /* 관리 화면에서 고른 블록 — 허브 버튼을 눌러야 진행 */
static int64_t s_arm_until;

bool hub_crypt_on(void) { return s_on; }
int hub_crypt_key_block(void) { return s_key; }

/* ---------------------------------------------------------------- eFuse */

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

/* cfg = NULL 이면 평문. allow_erase 면 '가득 참/버전 바뀜'일 때 지우고 새로 만든다(zb_nvs 평상시만 — Zigbee 는 재가입으로
 * 복구되지만, 앱 nvs 는 Wi-Fi·관리자 설정 전체라 지우지 않고 복구 모드로 넘긴다). */
static esp_err_t open_part(const char *name, nvs_sec_cfg_t *cfg, bool allow_erase, bool *reset)
{
    esp_err_t e = cfg ? nvs_flash_secure_init_partition(name, cfg) : nvs_flash_init_partition(name);
    if (allow_erase && (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
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
    if (err == ESP_OK && r != ESP_ERR_NVS_NOT_FOUND) {
        err = r;                                   /* 열거를 시작/계속하지 못함(메모리 부족 등) — 빈 목록을 성공으로 보지 않는다 */
        ESP_LOGE(TAG, "%s 항목 열거 실패(%s)", part, esp_err_to_name(r));
    }
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

/* 두 파티션을 평문으로 열어 다 읽는다(여기서는 지우지 않는다 — 실패하면 그만) */
static esp_err_t collect_both(ent_t **a, ent_t **b, size_t *total)
{
    int na = 0, nb = 0;
    *total = 0;
    esp_err_t e = open_part(PART_ZB, NULL, false, NULL);
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

/* 파티션을 다시 읽어 목록과 하나하나 같은지(되쓰기 검증) */
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
            ESP_LOGE(TAG, "검증: %s/%s:%s 다름", part, x->ns, x->key);
            ok = false;
        }
    }
    free_list(r);
    ESP_LOGW(TAG, "검증: %s 항목 %d개 %s(읽은 것 %d개)", part, m, ok && n == m ? "같음" : "다름", n);
    return ok && n == m;
}

/* ---------------------------------------------------------------- 전환 저널 + 마커 (ota_1)
 * ota_1 은 OTA 를 쓰지 않아 비어 있다. 앞부분 = 전환 저널(원본 그대로의 백업 + 진행 단계), 마지막 4KB = 영구 마커.
 *   저널  [헤더 4KB][nvs 원본 24KB][zb_nvs 원본 64KB]   — 전환 중에만 있고, 끝나면 지운다(평문 비밀값이 남지 않게)
 *   단계  state 낱말의 비트를 지우지 않고 1→0 으로만 바꿔 기록: 백업됨 → 키 구움 → 완료
 *   마커  "우리가 KEY n 으로 암호화했다" — 부팅 때 이것이 있어야 암호화로 연다(다른 용도의 HMAC 키와 구분)
 * 나중에 OTA 를 넣으면 이 영역을 피해야 한다. */
#define J_MAGIC  "LYNCRJ01"
#define M_MAGIC  "LYNCRM01"
#define ST_BACKUP 0xFFFFFFFFu
#define ST_KEY    0xFFFF0000u
#define ST_DONE   0x00000000u
#define J_SEC     4096
#define J_TRIES   3
#define J_TEST    7                  /* 시험 빌드: 되돌리기만 하고 굽지 않는 시험 저널 */

typedef struct {
    char magic[8];
    uint32_t key, nvs_size, zb_size, nvs_crc, zb_crc;
    uint32_t hdr_crc;                    /* 위 필드들의 CRC(state·tries 는 빼고 — 이 둘은 비트만 지워 가며 바꾼다) */
    uint32_t state;
    uint32_t tries;                      /* 복구 시도마다 비트 하나씩 0 으로 */
} jhdr_t;

typedef struct {
    char magic[8];
    uint32_t key, crc;
} marker_t;

static uint32_t crc32b(uint32_t c, const void *d, size_t n)
{
    const uint8_t *p = d;
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
    }
    return ~c;
}

static const esp_partition_t *ota1(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
}

static const esp_partition_t *data_part(const char *label)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, label);
}

/* 마커는 두 벌(마지막 섹터, 그 앞 섹터) — 한 벌이 깨져도 평문으로 잘못 열지 않게(그러면 암호화된 항목이 다 지워진다) */
#define M_OFF(o, n) ((o)->size - J_SEC * (size_t)((n) + 1))

static int marker_read(int n)
{
    const esp_partition_t *o = ota1();
    marker_t m;
    if (!o || esp_partition_read(o, M_OFF(o, n), &m, sizeof(m)) != ESP_OK || memcmp(m.magic, M_MAGIC, 8) ||
        m.key > 5 || m.crc != crc32b(0, &m, offsetof(marker_t, crc))) {
        return -1;
    }
    return (int)m.key;
}

static esp_err_t marker_put(int n, int key)
{
    const esp_partition_t *o = ota1();
    marker_t m = { .key = (uint32_t)key };
    memcpy(m.magic, M_MAGIC, 8);
    m.crc = crc32b(0, &m, offsetof(marker_t, crc));
    esp_err_t e = esp_partition_erase_range(o, M_OFF(o, n), J_SEC);
    return e == ESP_OK ? esp_partition_write(o, M_OFF(o, n), &m, sizeof(m)) : e;
}

static esp_err_t marker_write(int key)
{
    esp_err_t e = marker_put(0, key);
    return e == ESP_OK ? marker_put(1, key) : e;
}

/* 어느 한 벌이라도 맞으면 그 키. 한 벌만 맞으면 다른 벌을 고쳐 둔다 */
static int marker_key(void)
{
    int a = marker_read(0), b = marker_read(1);
    int k = a >= 0 ? a : b;
    if (k >= 0 && (a != k || b != k)) {
        ESP_LOGW(TAG, "암호화 표시 한 벌이 깨져 있어 고침");
        marker_put(a != k ? 0 : 1, k);
    }
    return k;
}

static bool journal_read(jhdr_t *h)
{
    const esp_partition_t *o = ota1();
    return o && esp_partition_read(o, 0, h, sizeof(*h)) == ESP_OK && !memcmp(h->magic, J_MAGIC, 8) &&
           h->hdr_crc == crc32b(0, h, offsetof(jhdr_t, hdr_crc));
}

static esp_err_t journal_set(size_t field_off, uint32_t v)
{
    return esp_partition_write(ota1(), field_off, &v, sizeof(v));   /* 비트를 1→0 으로만(지우지 않고 덧씀) */
}

static size_t journal_len(void)
{
    const esp_partition_t *a = data_part(PART_MAIN), *b = data_part(PART_ZB);
    size_t len = J_SEC + (a ? a->size : 0) + (b ? b->size : 0);
    return (len + J_SEC - 1) / J_SEC * J_SEC;
}

/* 평문 백업(데이터)부터 지우고 헤더는 마지막에 — 중간에 끊기면 헤더가 남아 다음 부팅이 다시 지운다 */
static esp_err_t journal_erase(void)
{
    const esp_partition_t *o = ota1();
    esp_err_t e = esp_partition_erase_range(o, J_SEC, journal_len() - J_SEC);
    return e == ESP_OK ? esp_partition_erase_range(o, 0, J_SEC) : e;
}

/* 헤더 없이 남은 백업 조각(백업 도중 끊김 등)이 있나 — 있으면 Wi-Fi 암호 등 평문이 남아 있는 것 */
static bool journal_dirty(void)
{
    const esp_partition_t *o = ota1();
    uint32_t *buf = o ? malloc(J_SEC) : NULL;
    bool dirty = false;
    for (size_t off = 0; buf && !dirty && off < journal_len(); off += J_SEC) {
        if (esp_partition_read(o, off, buf, J_SEC) != ESP_OK) {
            break;
        }
        for (int i = 0; i < J_SEC / 4; i++) {
            if (buf[i] != 0xFFFFFFFFu) {
                dirty = true;
                break;
            }
        }
    }
    free(buf);
    return dirty;
}

/* 파티션 src 를 4KB 씩 ota_1 의 off 로 복사(dir=0) 하거나 되돌린다(dir=1). crc 를 계산해 돌려준다 */
static esp_err_t raw_copy(const esp_partition_t *p, size_t off, bool back, uint32_t *crc)
{
    const esp_partition_t *o = ota1();
    uint8_t *buf = malloc(J_SEC);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t e = back ? esp_partition_erase_range(p, 0, p->size) : ESP_OK;
    uint32_t c = 0;
    for (size_t i = 0; e == ESP_OK && i < p->size; i += J_SEC) {
        e = back ? esp_partition_read(o, off + i, buf, J_SEC) : esp_partition_read(p, i, buf, J_SEC);
        if (e == ESP_OK) {
            c = crc32b(c, buf, J_SEC);
            e = back ? esp_partition_write(p, i, buf, J_SEC) : esp_partition_write(o, off + i, buf, J_SEC);
        }
    }
    free(buf);
    if (crc) {
        *crc = c;
    }
    return e;
}

/* 1단계: 두 파티션을 원본 그대로 ota_1 에 백업하고 다시 읽어 CRC 확인 */
static esp_err_t journal_backup(int key)
{
    const esp_partition_t *o = ota1(), *a = data_part(PART_MAIN), *b = data_part(PART_ZB);
    if (!o || !a || !b || o->size < J_SEC * 2 + a->size + b->size) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t e = journal_erase();
    jhdr_t h = { .key = (uint32_t)key, .nvs_size = a->size, .zb_size = b->size, .state = ST_BACKUP, .tries = 0xFFFFFFFFu };
    memcpy(h.magic, J_MAGIC, 8);
    if (e == ESP_OK) e = raw_copy(a, J_SEC, false, &h.nvs_crc);
    if (e == ESP_OK) e = raw_copy(b, J_SEC + a->size, false, &h.zb_crc);
    /* 다시 읽어 확인 */
    uint8_t *buf = e == ESP_OK ? malloc(J_SEC) : NULL;
    uint32_t c1 = 0, c2 = 0;
    for (size_t i = 0; buf && e == ESP_OK && i < a->size + b->size; i += J_SEC) {
        e = esp_partition_read(o, J_SEC + i, buf, J_SEC);
        if (i < a->size) c1 = crc32b(c1, buf, J_SEC); else c2 = crc32b(c2, buf, J_SEC);
    }
    free(buf);
    if (e == ESP_OK && (c1 != h.nvs_crc || c2 != h.zb_crc)) {
        e = ESP_ERR_INVALID_CRC;
    }
    if (e == ESP_OK) {
        h.hdr_crc = crc32b(0, &h, offsetof(jhdr_t, hdr_crc));
        e = esp_partition_write(o, 0, &h, sizeof(h));   /* 헤더는 마지막에 — 헤더가 있으면 백업이 온전하다 */
    }
    return e;
}

/* 백업을 두 파티션에 되돌린다(원본 평문 상태로) */
static esp_err_t journal_restore(const jhdr_t *h)
{
    const esp_partition_t *a = data_part(PART_MAIN), *b = data_part(PART_ZB);
    uint32_t c1, c2;
    nvs_flash_deinit_partition(PART_MAIN);
    nvs_flash_deinit_partition(PART_ZB);
    esp_err_t e = raw_copy(a, J_SEC, true, &c1);
    if (e == ESP_OK) e = raw_copy(b, J_SEC + h->nvs_size, true, &c2);
    if (e == ESP_OK && (c1 != h->nvs_crc || c2 != h->zb_crc)) {
        e = ESP_ERR_INVALID_CRC;
    }
    return e;
}

/* 2~4단계(평문 두 파티션이 열려 있다고 보고): 다 읽기 → (필요하면) 굽기 → 암호화로 되쓰기 → 다시 읽어 검증.
 * 성공하면 완료 → 마커 → 저널 지우기. 실패하면 false(저널은 남아 다음 부팅에 되돌려 다시 시도) */
static bool encrypt_from_plain(int key)
{
    ent_t *a = NULL, *b = NULL;
    size_t total;
    if (collect_both(&a, &b, &total) != ESP_OK) {
        ESP_LOGE(TAG, "데이터를 다 읽지 못함(%u바이트 한도)", (unsigned)MIGRATE_MAX);
        return false;
    }
    bool ok = false;
    esp_efuse_block_t blk = EFUSE_BLK_KEY0 + key;
    if (esp_efuse_key_block_unused(blk)) {
        uint8_t k[32];
        bootloader_random_enable();                /* Wi-Fi 전이라 하드웨어 난수원을 켜고 */
        esp_fill_random(k, sizeof(k));
        bootloader_random_disable();
        esp_err_t e = esp_efuse_write_key(blk, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, k, sizeof(k));
        memset(k, 0, sizeof(k));
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "eFuse KEY%d 굽기 실패(%s)", key, esp_err_to_name(e));
            goto out;
        }
        ESP_LOGW(TAG, "eFuse KEY%d 에 HMAC 키를 구움(읽기·쓰기 금지됨)", key);
    } else if (esp_efuse_get_key_purpose(blk) != ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
        ESP_LOGE(TAG, "KEY%d 는 다른 용도로 쓰는 중", key);
        goto out;
    }
    journal_set(offsetof(jhdr_t, state), ST_KEY);
    nvs_sec_cfg_t cfg;
    if (sec_cfg_hmac(key, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HMAC 로 암호화 키를 못 만듦");
        goto out;
    }
    if (rewrite(PART_MAIN, &cfg, a) != ESP_OK || rewrite(PART_ZB, &cfg, b) != ESP_OK) {
        ESP_LOGE(TAG, "암호화로 되쓰기 실패");
        goto out;
    }
    if (!same(PART_MAIN, a) || !same(PART_ZB, b)) {      /* 암호화된 채로 다시 읽어 하나하나 비교 */
        ESP_LOGE(TAG, "암호화 후 검증 실패");
        goto out;
    }
    /* 완료 → 마커 → 저널(평문 백업) 지우기. 사이에 꺼져도 부팅이 이어서 마무리한다 */
    if (journal_set(offsetof(jhdr_t, state), ST_DONE) != ESP_OK || marker_write(key) != ESP_OK) {
        ESP_LOGE(TAG, "완료 기록 실패");
        goto out;
    }
    journal_erase();
    ok = true;
out:
    free_list(a);
    free_list(b);
    return ok;
}

/* 앱 nvs 를 열 수 없을 때: 지우지 않고 USB 로 사람이 고르게 한다(빨간 LED 깜빡임) */
static void recovery_console(const char *why)
{
    hub_led_alarm();                               /* 빨강 4번 깜빡(LED 는 이미 켜져 있다) */
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    char line[32];
    int len = 0;
    int64_t next = 0;
    for (;;) {
        if (esp_timer_get_time() >= next) {
            next = esp_timer_get_time() + 5000000;
            printf("\n#복구 모드: 설정 저장소(nvs)를 열 수 없음 — %s\n"
                   "#  'nvs erase' = 설정을 지우고 첫 설정부터(Zigbee 가입은 유지)   'reboot' = 다시 시도\n", why);
            fflush(stdout);
        }
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(200)) != 1) {
            continue;
        }
        if (c != '\n' && c != '\r') {
            if (len < (int)sizeof(line) - 1) {
                line[len++] = (char)c;
            }
            continue;
        }
        line[len] = 0;
        len = 0;
        if (!strcmp(line, "nvs erase")) {
            printf("#설정 저장소를 지우고 재시작\n");
            nvs_flash_erase_partition(PART_MAIN);
            esp_restart();
        } else if (!strcmp(line, "reboot")) {
            esp_restart();
        }
    }
}

#if HUB_CRYPT_SELFTEST
/* 시험: 고정 키로 암호화 → 다시 읽어 비교 → 평문으로 되돌림 → 비교. eFuse 는 건드리지 않는다. */
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
    ok = ok && same(PART_MAIN, a) && same(PART_ZB, b);
    bool back = rewrite(PART_MAIN, NULL, a) == ESP_OK && rewrite(PART_ZB, NULL, b) == ESP_OK;
    back = back && same(PART_MAIN, a) && same(PART_ZB, b);
    ESP_LOGW(TAG, "시험 결과: 암호화 왕복 %s, 평문 복원 %s", ok ? "통과" : "실패", back ? "통과" : "실패");
    free_list(a);
    free_list(b);
    /* 전원 끊김 시험: 원본 백업(시험 키 번호 J_TEST) → 암호화로 되쓰다 '끊긴' 것처럼 재시작 → 부팅이 백업을 되돌려야 한다 */
    if (ok && back && journal_backup(J_TEST) == ESP_OK) {
        nvs_sec_cfg_t c2;
        memset(c2.eky, 0x33, sizeof(c2.eky));
        memset(c2.tky, 0x44, sizeof(c2.tky));
        nvs_flash_deinit_partition(PART_MAIN);
        nvs_flash_deinit_partition(PART_ZB);
        nvs_flash_erase_partition(PART_MAIN);       /* 지운 채(= 되쓰기 도중) 끊김 */
        nvs_flash_secure_init_partition(PART_ZB, &c2);
        ESP_LOGW(TAG, "시험: 전환 도중 끊김 흉내 — 재시작");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
}
#endif

/* ---------------------------------------------------------------- 부팅 */

static esp_err_t open_main(nvs_sec_cfg_t *cfg)
{
    esp_err_t e = open_part(PART_MAIN, cfg, false, NULL);
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        recovery_console(esp_err_to_name(e));      /* 돌아오지 않음 */
    }
    return e;
}

esp_err_t hub_crypt_boot(bool *zb_reset)
{
    /* 1) 끝나지 않은 전환이 있으면: 백업을 되돌리고 이어서(최대 J_TRIES 번), 넘으면 평문으로 되돌리고 포기 */
    jhdr_t jh;
    if (journal_read(&jh)) {
        if (jh.state == ST_DONE) {                 /* 끝났는데 마무리 전에 꺼짐: 마커 확인 후 평문 백업 지우기 */
            if (marker_key() != (int)jh.key) {
                marker_write((int)jh.key);
            }
            journal_erase();
            ESP_LOGW(TAG, "전환 마무리(평문 백업 지움)");
        } else if (jh.key == J_TEST) {
            esp_err_t e = journal_restore(&jh);
            ESP_LOGW(TAG, "시험 저널: 백업에서 되돌림 %s(%s)", e == ESP_OK ? "통과" : "실패", esp_err_to_name(e));
            if (e != ESP_OK) {
                recovery_console("시험 백업을 되돌리지 못함");
            }
            journal_erase();
        } else {
            int used = 0;
            for (uint32_t t = jh.tries; !(t & 1) && used < 32; t >>= 1) {
                used++;
            }
            ESP_LOGW(TAG, "끝나지 않은 암호화 전환 발견(KEY%d, 시도 %d) — 백업에서 되돌림", (int)jh.key, used + 1);
            if (journal_restore(&jh) != ESP_OK) {
                recovery_console("전환 백업을 되돌리지 못함");
            }
            if (used >= J_TRIES) {
                ESP_LOGE(TAG, "%d번 실패 — 평문으로 되돌린 채 포기", J_TRIES);
                journal_erase();
            } else {
                journal_set(offsetof(jhdr_t, tries), jh.tries & ~(1u << used));
                if (open_main(NULL) == ESP_OK && encrypt_from_plain((int)jh.key)) {
                    ESP_LOGW(TAG, "암호화 전환 완료 — 재시작");
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
                esp_restart();                     /* 성공이든 실패든 깨끗하게 다시(실패면 다음 부팅에 또 되돌림) */
            }
        }
    } else if (journal_dirty()) {                    /* 헤더 없는 백업 조각: 평문 비밀값이 남지 않게 지운다 */
        ESP_LOGW(TAG, "남은 백업 조각 지움");
        journal_erase();
    }

    /* 2) 우리가 암호화해 둔 표시(마커)가 있으면 그 키로 연다 — 다른 용도의 HMAC 키만으로는 판정하지 않는다 */
    int key = marker_key();
    if (key >= 0) {
        if (esp_efuse_get_key_purpose(EFUSE_BLK_KEY0 + key) != ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
            recovery_console("암호화 표시는 있는데 eFuse 키가 없음");
        }
        nvs_sec_cfg_t cfg;
        esp_err_t e = sec_cfg_hmac(key, &cfg);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "eFuse KEY%d HMAC 로 저장소 키를 못 만듦(%s)", key, esp_err_to_name(e));
            return e;
        }
        e = open_main(&cfg);
        if (e == ESP_OK) {
            e = open_part(PART_ZB, &cfg, true, zb_reset);
        }
        if (e == ESP_OK) {
            s_on = true;
            s_key = key;
            ESP_LOGI(TAG, "저장 데이터 암호화 켜짐(eFuse KEY%d, HMAC)", key);
        }
        return e;
    }

    /* 3) 평문 */
    esp_err_t e = open_main(NULL);
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
        clear_request();                           /* 요청은 지우고(백업에도 안 남게) */
        if (journal_backup((int)req) != ESP_OK) {
            ESP_LOGE(TAG, "원본 백업 실패 — 굽지 않고 그만");
            journal_erase();
        } else {
            if (encrypt_from_plain((int)req)) {
                ESP_LOGW(TAG, "암호화 전환 완료 — 재시작");
            } else {
                ESP_LOGE(TAG, "전환 실패 — 재시작하면 백업에서 되돌려 다시 시도");
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        }
    }
    return open_part(PART_ZB, NULL, true, zb_reset);
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
    strlcat(o, "<p class=muted>막는 범위: 허브를 가져가 USB 로 플래시를 통째로 읽어 가는 것(덤프). 보안 부팅이 없으므로 "
               "USB 로 다른 펌웨어를 올리면 같은 칩 키로 풀 수 있습니다. 암호화한 허브에 이 기능이 없는 옛 펌웨어를 올리면 "
               "저장 데이터가 지워집니다.</p>", cap);
    if (!s_on && esp_timer_get_time() < s_arm_until) {
        snprintf(t, sizeof(t), "<p class=warn><b>KEY%d 로 켜기를 기다리는 중 — 60초 안에 허브 버튼을 짧게 누르세요.</b></p>",
                 s_arm_key);
        strlcat(o, t, cap);
    }
    if (!s_on) {
        strlcat(o, "<form method=post action=/admin/crypt onsubmit=\"return confirm('고른 eFuse 블록에 키를 굽고 저장 데이터를 "
                   "암호화합니다. eFuse 는 되돌릴 수 없고 암호화는 끌 수 없습니다. 계속할까요?')\">"
                   "<p class=muted>키를 넣을 eFuse 블록을 고르세요. 비어 있으면 새 키를 굽고, 이미 HMAC 키가 있으면 그대로 씁니다.</p>",
                cap);
    }
    for (int k = 0; k < 6; k++) {
        esp_efuse_block_t b = EFUSE_BLK_KEY0 + k;
        bool unused = esp_efuse_key_block_unused(b);
        bool hmac = !unused && esp_efuse_get_key_purpose(b) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP;
        const char *st = unused ? "비어 있음 — 여기에 새 키를 굽습니다"
                         : hmac && !s_on ? "이미 HMAC 키 있음 — 굽지 않고 그대로 씀"
                                         : purpose_name(esp_efuse_get_key_purpose(b));
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
                   "<p class=muted>누른 뒤 60초 안에 <b>허브의 버튼을 짧게</b> 눌러야 진행합니다(허브 앞에 있는 사람만 켤 수 있게). "
                   "그러면 허브가 재시작하며 옮깁니다(수 초). 중간에 전원이 끊기면 다음 부팅에 원래대로 되돌린 뒤 다시 "
                   "시도합니다.</p><button class=sv>암호화 켜기</button></form>", cap);
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
    s_arm_key = k;
    s_arm_until = esp_timer_get_time() + 60LL * 1000000;
    ESP_LOGW(TAG, "암호화 켜기 요청(KEY%d) — 허브 버튼 확인 기다림(60초)", k);
    snprintf(msg, msglen, "60초 안에 허브의 버튼을 짧게 누르면 KEY%d 로 암호화를 시작합니다(허브가 재시작). "
             "누르지 않으면 취소됩니다", k);
    return true;
}

bool hub_crypt_button(void)
{
    if (s_on || esp_timer_get_time() >= s_arm_until) {
        return false;
    }
    s_arm_until = 0;
    nvs_handle_t h;
    esp_err_t e = nvs_open_from_partition(PART_MAIN, NS_CRYPT, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_i32(h, "pending", s_arm_key);
        if (e == ESP_OK) {
            e = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "암호화 요청을 저장하지 못함(%s)", esp_err_to_name(e));
        hub_led_set(HUB_LED_RED);
        return true;
    }
    ESP_LOGW(TAG, "버튼 확인 — KEY%d 로 암호화하러 재시작", s_arm_key);
    hub_led_set(HUB_LED_YELLOW);
    xTaskCreate(restart_task, "crypt_rst", 2048, NULL, 5, NULL);
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
