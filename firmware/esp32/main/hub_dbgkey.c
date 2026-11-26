/*
 * hub_dbgkey.c -- 원격 디버그 키(여러 개). 설명은 hub_dbgkey.h.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"

#include "hub_dbgkey.h"

static const char *TAG = "HUB_DBGKEY";

#define DK_MAX     6
#define DK_ALIAS   24
#define DK_KEYLEN  32          /* 16진 32자리 = 16바이트 */

typedef struct {
    char     alias[DK_ALIAS];  /* NUL 종료 */
    uint32_t created;          /* epoch, 0 = 모름(시계 전) */
    char     key[DK_KEYLEN];   /* 16진, NUL 없음 */
} dk_ent_t;

typedef struct {
    uint8_t  n;
    dk_ent_t e[DK_MAX];
} dk_blob_t;

static dk_blob_t s_db;
static bool s_loaded;

static void ensure_loaded(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    memset(&s_db, 0, sizeof(s_db));
    nvs_handle_t h;
    if (nvs_open("dbgkey", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_db);
        if (nvs_get_blob(h, "keys", &s_db, &sz) != ESP_OK || s_db.n > DK_MAX) {
            memset(&s_db, 0, sizeof(s_db));
        }
        nvs_close(h);
    }
}

static bool save(void)
{
    nvs_handle_t h;
    if (nvs_open("dbgkey", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e = nvs_set_blob(h, "keys", &s_db, sizeof(s_db));
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e == ESP_OK;
}

static uint32_t now_epoch(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (uint32_t)t : 0;   /* SNTP 동기 전이면 0 */
}

int hub_dbgkey_create(const char *alias, char out[33])
{
    ensure_loaded();
    if (s_db.n >= DK_MAX) {
        return 0;
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    dk_ent_t *e = &s_db.e[s_db.n];
    memset(e, 0, sizeof(*e));
    for (int i = 0; i < 16; i++) {
        static const char hx[] = "0123456789abcdef";
        e->key[2 * i]     = hx[rnd[i] >> 4];
        e->key[2 * i + 1] = hx[rnd[i] & 0xf];
    }
    if (alias && alias[0]) {
        strlcpy(e->alias, alias, sizeof(e->alias));
    } else {
        strlcpy(e->alias, "(이름 없음)", sizeof(e->alias));
    }
    e->created = now_epoch();
    s_db.n++;
    if (!save()) {
        s_db.n--;                      /* 저장 실패 → 되돌림 */
        return 0;
    }
    memcpy(out, e->key, DK_KEYLEN);
    out[DK_KEYLEN] = '\0';
    ESP_LOGW(TAG, "디버그 키 생성: '%s' (총 %d개)", e->alias, s_db.n);
    return 1;
}

int hub_dbgkey_delete(int idx)
{
    ensure_loaded();
    if (idx < 0 || idx >= s_db.n) {
        return 0;
    }
    for (int i = idx; i + 1 < s_db.n; i++) {
        s_db.e[i] = s_db.e[i + 1];
    }
    s_db.n--;
    memset(&s_db.e[s_db.n], 0, sizeof(s_db.e[s_db.n]));
    return save() ? 1 : 0;
}

bool hub_dbgkey_check(httpd_req_t *req)
{
    ensure_loaded();
    if (s_db.n == 0) {
        return false;
    }
    char hdr[80];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK ||
        strncmp(hdr, "Bearer ", 7) != 0 || strlen(hdr + 7) != DK_KEYLEN) {
        return false;
    }
    const char *tok = hdr + 7;
    bool ok = false;
    for (int k = 0; k < s_db.n; k++) {
        uint8_t d = 0;
        for (int i = 0; i < DK_KEYLEN; i++) {
            d |= (uint8_t)(tok[i] ^ s_db.e[k].key[i]);
        }
        if (!d) {
            ok = true;             /* break 없이 끝까지 — 걸리는 시간이 키에 따라 달라지지 않게 */
        }
    }
    return ok;
}

static void cat_esc(char *o, size_t cap, const char *s)
{
    char buf[DK_ALIAS * 6 + 1];
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 6 < sizeof(buf); i++) {
        char c = s[i];
        const char *r = c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : c == '"' ? "&quot;" : NULL;
        if (r) {
            size_t n = strlen(r);
            memcpy(buf + j, r, n);
            j += n;
        } else {
            buf[j++] = c;
        }
    }
    buf[j] = '\0';
    strlcat(o, buf, cap);
}

/* 카드 안쪽(안내·만들기·목록). 관리 화면 카드와 AJAX 응답(/admin/dbgkey, X-Ajax)이 같이 쓴다.
 * new_key 가 있으면 그 값을 이번 한 번만 보여 준다. note 는 결과 한 줄(고정 문구). */
void hub_dbgkey_render_body(char *out, size_t cap, const char *new_key, const char *note, bool ok)
{
    ensure_loaded();
    char t[448];
    if (new_key) {
        snprintf(t, sizeof(t),
                 "<p class=ok>새 디버그 키 — <b>지금만 보입니다</b>, 복사해 두세요.<br>"
                 "<code id=dkval style=\"user-select:all;word-break:break-all\">%s</code> "
                 "<button type=button class=lnk onclick=\"dkCopy(this)\">복사</button></p>", new_key);
        strlcat(out, t, cap);
    } else if (note && note[0]) {
        snprintf(t, sizeof(t), "<p class=%s>%s</p>", ok ? "ok" : "warn", note);
        strlcat(out, t, cap);
    }
    strlcat(out,
            "<p class=muted>펌웨어 올리기(OTA)·재부팅·진단을 집 밖이나 스크립트에서 쓰려는 키입니다. "
            "요청 헤더 <code>Authorization: Bearer &lt;키&gt;</code> 로 씁니다. "
            "<b>만들 때 한 번만</b> 보이고, 이후엔 별명·만든 날짜만 보입니다. 고치기는 없고 삭제만 됩니다.</p>", cap);

    if (s_db.n >= DK_MAX) {
        strlcat(out, "<p class=warn>키가 가득 찼습니다(6개). 쓰지 않는 키를 지우고 만드세요.</p>", cap);
    } else {
        /* 페이지를 옮기지 않고 만든다(옮기면 새로고침할 때마다 폼이 다시 보내져 키가 또 생긴다) */
        strlcat(out,
                "<form method=post action=/admin/dbgkey onsubmit=\"return dkNew(this)\">"
                "<input name=alias maxlength=23 placeholder=\"별명 (예: claude-debug)\">"
                "<button name=op value=new class=sv>키 만들기</button></form>", cap);
    }

    if (s_db.n == 0) {
        strlcat(out, "<p class=muted>아직 만든 키가 없습니다.</p>", cap);
    } else {
        strlcat(out, "<table style=\"width:100%;margin-top:8px\"><tr><th align=left>별명</th>"
                     "<th align=left>만든 날짜</th><th></th></tr>", cap);
        for (int i = 0; i < s_db.n; i++) {
            char date[24] = "—";
            if (s_db.e[i].created) {
                time_t ct = s_db.e[i].created;
                struct tm tm;
                localtime_r(&ct, &tm);
                strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &tm);
            }
            strlcat(out, "<tr><td>", cap);
            cat_esc(out, cap, s_db.e[i].alias);
            snprintf(t, sizeof(t),
                     "</td><td>%s</td><td align=right>"
                     "<button type=button class=ghost onclick=\"dkDel(%d)\">삭제</button></td></tr>",
                     date, i);
            strlcat(out, t, cap);
        }
        strlcat(out, "</table>", cap);
    }
}

void hub_dbgkey_render(char *out, size_t cap)
{
    strlcat(out, "<h2>원격 디버그 키</h2><div class=card id=dbgk>", cap);
    hub_dbgkey_render_body(out, cap, NULL, NULL, true);
    strlcat(out, "</div>", cap);
    /* 만들기·삭제는 fetch 로 보내고 카드 안쪽만 바꾼다 — 주소가 /admin 그대로라 새로고침해도 다시 보내지지 않는다 */
    strlcat(out,
            "<script>function dkPost(b){var c=document.getElementById('dbgk');"
            "fetch('/admin/dbgkey',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded','X-Ajax':'1'},body:b})"
            ".then(function(r){return r.text().then(function(t){c.innerHTML=r.ok?t:'<p class=warn>'+t+'</p>'})})"
            ".catch(function(){c.insertAdjacentHTML('afterbegin','<p class=warn>허브에 닿지 않습니다</p>')});}"
            "function dkNew(f){dkPost('op=new&alias='+encodeURIComponent(f.alias.value));return false}"
            "function dkDel(i){if(confirm('이 키를 삭제할까요? 그 키로 접속하던 쪽은 바로 끊깁니다.'))dkPost('op=del&idx='+i)}"
            "function dkCopy(b){var e=document.getElementById('dkval');if(!e)return;"
            "var r=document.createRange();r.selectNodeContents(e);var g=getSelection();g.removeAllRanges();g.addRange(r);"
            "var ok=false;try{ok=document.execCommand('copy')}catch(x){}"        /* HTTP(비보안)에선 이게 실제 복사 */
            "if(navigator.clipboard&&navigator.clipboard.writeText){navigator.clipboard.writeText(e.textContent).then(function(){},function(){});ok=true}"
            "if(b){b.textContent=ok?'복사됨':'길게 눌러 복사';setTimeout(function(){b.textContent='복사'},1500)}}"
            "</script>", cap);
}
