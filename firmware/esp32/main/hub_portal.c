/*
 * hub_portal.c -- 설정 포털(esp_http_server, :80). Pi 판 appliance/portal/portal.py 를 옮긴 것.
 *
 *  - 미설정: 첫 화면 = 설정 폼. 모르는 주소는 전부 설정 페이지로 302(캡티브: 폰이 접속하면 자동으로 뜸).
 *  - 설정 완료 응답 후 2초 뒤 Wi-Fi 를 다시 적용(설정 AP 닫고 집 Wi-Fi + 전용 AP), DNS 는 클라우드 이름만.
 *  - 설정 완료 때 세션을 주지 않는다: 설정용 망에선 초기 설정 한 번이 끝. 관리 화면은 다음 단계.
 *  - 완료 화면에 암호를 보여주지 않는다(직접 입력한 값이므로).
 *  - 설정 후: /admin = 관리 화면(관리자 암호로 로그인, 세션 쿠키 30분). 지금은 플러그인 추가·삭제·핀 설정.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hub_dns.h"
#include "hub_portal.h"
#include "hub_wifi.h"
#include "hub_events.h"
#include "hub_sensors.h"
#include "hub_proxy.h"
#include "hub_crypt.h"
#include "hub_tariff.h"
#include "hub_ota.h"
#include "hub_dbgkey.h"
#include "hub_devs.h"
#include "hub_zigbee.h"
#include "hub_plugins.h"
#include "hub_expose.h"
#include "hub_uplink.h"
#include "hub_rinnai.h"
#include "hub_store.h"
#include "nvs.h"
#include "hub_api.h"
#include "hub_pairs.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "HUB_PORTAL";
static hub_state_t *s_st;

#define BODY_MAX 1024

/* ---------- 공통 HTML (Pi 판 page() + 칸별 눈 아이콘) ---------- */
static const char HEAD1[] =
    "<!doctype html><html lang=ko><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>";
static const char HEAD2[] =
    "</title><style>"
    "body{font-family:system-ui,sans-serif;max-width:560px;margin:0 auto;padding:16px;background:#111;color:#eee}"
    "h1{font-size:20px}label{display:block;margin:12px 0 4px;color:#bbb}"
    "input,button,form>select{font-size:16px;padding:10px;width:100%;box-sizing:border-box;border-radius:8px;"
    "border:1px solid #444;background:#1c1c1c;color:#eee}"
    "button{background:#c33;border:0;margin-top:14px;font-weight:600}button:disabled{opacity:.45}"
    ".card{background:#191919;border:1px solid #333;border-radius:12px;padding:16px;margin:14px 0}"
    ".muted{color:#888;font-size:13px}.warn{color:#f88}"
    ".pwwrap{position:relative;display:block}.pwwrap input{padding-right:48px}"
    "button.eye{position:absolute;right:4px;top:50%;transform:translateY(-50%);width:40px;height:40px;"
    "margin:0;padding:8px;background:transparent;border:0;color:#aaa;line-height:0}"
    "h2{font-size:16px;margin-top:24px}h3{font-size:15px;margin:0 0 6px}a{color:#8af}"
    "button.ghost{background:#2a2a2a;border:1px solid #555}.ok{color:#8d8}"
    "button.lnk{width:auto;display:inline;background:none;border:0;padding:0;margin:0;color:#8af;font:inherit;cursor:pointer}"
    "button.ic{width:auto;display:inline-block;margin:0;padding:6px 9px;background:#2a2a2a;border:1px solid #555;font-weight:400;vertical-align:middle}"
    "button.sv{background:#2a2a2a;border:1px solid #555;color:#999}button.sv.on{background:#c33;border:1px solid #c33;color:#fff}"
    ".chg{outline:1px solid #666;outline-offset:3px;border-radius:6px}"
    "dialog{background:#1c1c1c;color:#eee;border:1px solid #555;border-radius:12px;padding:16px;width:min(320px,90vw)}"
    "dialog::backdrop{background:#000a}.dbtn{display:flex;gap:8px}"
    ".ptitle{display:flex;justify-content:space-between;align-items:center;gap:8px}.ptitle h3{margin:0}.ptitle form{margin:0}"
    ".row{display:flex;flex-wrap:wrap;justify-content:space-between;gap:6px;padding:6px 0;border-top:1px solid #2a2a2a}"
    "label.chk{display:inline-block;margin:0 0 0 10px;color:#ddd}label.chk input{width:auto;margin-right:4px}"
    "h3.grp{margin:14px 0 2px;color:#ccc}"
    ".grpbox{background:#202020;border:1px solid #3a3a3a;border-radius:10px;padding:10px 12px;margin:12px 0}"
    ".grpbox h3.grp{margin:0;padding-bottom:6px;color:#eee}.grpbox .sub{margin:0 0 6px;font-size:12px}"
    "</style></head><body>";
static const char TAIL[] =
    "<script>(function(){"
    "var O='<svg viewBox=\"0 0 24 24\" width=\"22\" height=\"22\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\""
    " stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\"><path d=\"M1 12s4-7 11-7 11 7 11 7-4 7-11 7S1 12 1 12z\"/>"
    "<circle cx=\"12\" cy=\"12\" r=\"3\"/></svg>',"
    "C='<svg viewBox=\"0 0 24 24\" width=\"22\" height=\"22\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\""
    " stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\"><path d=\"M17.94 17.94A10.9 10.9 0 0 1 12 19c-7 0-11-7-11-7a20.3 20.3 0 0 1 5.06-5.94\"/>"
    "<path d=\"M9.9 4.24A10.4 10.4 0 0 1 12 4c7 0 11 7 11 7a20.4 20.4 0 0 1-3.17 4.19\"/>"
    "<path d=\"M14.12 14.12a3 3 0 1 1-4.24-4.24\"/><path d=\"M1 1l22 22\"/></svg>';"
    "document.querySelectorAll('input[type=password]').forEach(function(i){"
    "var w=document.createElement('span');w.className='pwwrap';i.parentNode.insertBefore(w,i);w.appendChild(i);"
    "var b=document.createElement('button');b.type='button';b.className='eye';b.innerHTML=O;"
    "b.setAttribute('aria-label','암호 보기');"
    "b.onclick=function(){var v=i.type==='password';i.type=v?'text':'password';b.innerHTML=v?C:O;"
    "b.setAttribute('aria-label',v?'암호 숨기기':'암호 보기');i.focus();};w.appendChild(b);});})();</script>"
    "</body></html>";

/* 설정 폼의 실시간 검사: 허브 Wi-Fi 암호 8자↑, 관리자 암호 8자↑ + 확인 일치일 때만 버튼 활성 */
static const char CHK_JS[] =
    "<script>function _chk(){var w=document.getElementById('appw').value,a=document.getElementById('pw').value,"
    "b=document.getElementById('pw2').value,m=document.getElementById('pwmsg'),x=document.getElementById('btn');"
    "if(w.length<8){m.textContent='허브 Wi-Fi 암호는 8자 이상';m.className='muted';x.disabled=true;}"
    "else if(a.length<8){m.textContent='관리자 암호는 8자 이상';m.className='muted';x.disabled=true;}"
    "else if(a!==b){m.textContent='관리자 암호 확인 불일치';m.className='warn';x.disabled=true;}"
    "else{m.textContent='일치합니다 ✓';m.className='muted';x.disabled=false;}}</script>";

/* HTML 이스케이프해서 덧붙인다 */
static void cat_esc(char *out, size_t cap, const char *s)
{
    size_t o = strlen(out);
    for (; *s && o + 7 < cap; s++) {
        const char *r = NULL;
        switch (*s) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '"': r = "&quot;"; break;
        case '\'': r = "&#39;"; break;
        default: out[o++] = *s; out[o] = '\0'; continue;
        }
        strlcpy(out + o, r, cap - o);
        o += strlen(r);
    }
}

static void common_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
}

/* title + body(동적) 를 공통 머리/꼬리로 감싸 '한 번에' 보낸다.
 * 길이(Content-Length)를 밝히고 연결을 닫는다 — 조각 전송(chunked)보다 폰 OS 의 인터넷 확인이 덜 헷갈린다
 * (Pi 판도 이 방식이었고 캡티브 팝업이 떴다). */
static esp_err_t send_page(httpd_req_t *req, const char *title, const char *body, const char *extra)
{
    size_t n = strlen(HEAD1) + strlen(title) + strlen(HEAD2) + strlen(body) + (extra ? strlen(extra) : 0) + strlen(TAIL);
    char *buf = malloc(n + 1);
    if (!buf) {
        return httpd_resp_send_500(req);
    }
    buf[0] = '\0';
    strcat(buf, HEAD1); strcat(buf, title); strcat(buf, HEAD2); strcat(buf, body);
    if (extra) {
        strcat(buf, extra);
    }
    strcat(buf, TAIL);
    common_headers(req);
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t r = httpd_resp_send(req, buf, n);
    free(buf);
    return r;
}

static void log_req(httpd_req_t *req)
{
    char host[64] = "-", ua[48] = "-";
    httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
    httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));
    ESP_LOGI(TAG, "[req] fd %d %s %s%s  ua=%s", httpd_req_to_sockfd(req),
             req->method == HTTP_POST ? "POST" : "GET", host, req->uri, ua);
}

/* 연결이 열리고 닫히는 것 자체를 기록(요청이 안 보일 때 TCP 가 안 오는지, 와도 요청이 없는지 가르기 위해) */
static esp_err_t on_open(httpd_handle_t hd, int fd)
{
    struct sockaddr_in6 a;
    socklen_t al = sizeof(a);
    char ip[16] = "?";
    if (getpeername(fd, (struct sockaddr *)&a, &al) == 0) {
        if (a.sin6_family == AF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&a)->sin_addr, ip, sizeof(ip));
        } else {
            inet_ntoa_r(a.sin6_addr.un.u32_addr[3], ip, sizeof(ip));
        }
    }
    ESP_LOGI(TAG, "[conn] 열림 fd %d from %s", fd, ip);
    return ESP_OK;
}

static void on_close(httpd_handle_t hd, int fd)
{
    ESP_LOGI(TAG, "[conn] 닫힘 fd %d", fd);
    close(fd);
}

/* ---------- 화면 ---------- */
static esp_err_t setup_form(httpd_req_t *req, const char *msg)
{
    char def_ap[HUB_SSID_MAX + 1];
    hub_state_default_ap_ssid(s_st, def_ap, sizeof(def_ap));
    char *b = calloc(1, 3072);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    strlcat(b, "<h1>초기 설정</h1>", 3072);
    if (msg) {
        strlcat(b, "<p class=warn>", 3072); cat_esc(b, 3072, msg); strlcat(b, "</p>", 3072);
    }
    strlcat(b, "<div class=card><p>허브를 처음 설정합니다. 저장하면 허브가 <b>집 Wi-Fi</b>에 연결되고, "
               "이 설정용 네트워크는 닫히며 <b>허브 전용 네트워크</b>가 뜹니다.</p>"
               "<form method=post action=/provision>"
               "<label>집 Wi-Fi 이름 (SSID)</label><input name=sta_ssid maxlength=32 autocapitalize=off>"
               "<label>집 Wi-Fi 암호</label><input name=sta_pass type=password maxlength=63>"
               "<label>허브 네트워크 이름</label><input name=ap_ssid maxlength=32 value=\"", 3072);
    cat_esc(b, 3072, def_ap);
    strlcat(b, "\"><label>허브 Wi-Fi 암호 (8자 이상)</label>"
               "<input name=appw id=appw type=password maxlength=63 oninput=_chk()>"
               "<label>관리자 암호 (8자 이상)</label><input name=pw id=pw type=password oninput=_chk()>"
               "<label>관리자 암호 확인</label><input name=pw2 id=pw2 type=password oninput=_chk()>"
               "<div id=pwmsg class=muted></div>"
               "<button id=btn disabled>저장하고 전환</button></form>"
               "<p class=muted>지금은 공개 설정용 네트워크입니다. 설정을 마치면 닫힙니다.</p></div>", 3072);
    char title[32] = "초기 설정";
    esp_err_t r = send_page(req, title, b, CHK_JS);
    free(b);
    return r;
}

static esp_err_t root_get(httpd_req_t *req)
{
    log_req(req);
    if (!s_st->provisioned) {
        return setup_form(req, NULL);
    }
    char b[512] = "<h1>LYNNAI 허브</h1><div class=card><p>기기 ";
    cat_esc(b, sizeof(b), s_st->device_id);
    strlcat(b, " · 설정 완료</p><p><a href=/admin>관리 화면</a> (플러그인·센서 핀)</p></div>", sizeof(b));
    return send_page(req, "LYNNAI 허브", b, NULL);
}

/* ---------- 설정 저장 ---------- */
static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

static void form_get(const char *body, const char *key, char *out, size_t len)
{
    if (httpd_query_key_value(body, key, out, len) != ESP_OK) {
        out[0] = '\0';
    } else {
        url_decode(out);
    }
}

static void apply_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(2000));       /* 응답이 폰에 도착한 뒤 전환 */
    hub_dns_set_captive(false);
    hub_wifi_apply(s_st);
    hub_zigbee_allow_steering();           /* 이제 SmartThings 에서 '기기 추가'로 붙일 수 있다 */
    vTaskDelete(NULL);
}

static esp_err_t provision_post(httpd_req_t *req)
{
    log_req(req);
    if (s_st->provisioned) {
        httpd_resp_set_status(req, "303 See Other");
        httpd_resp_set_hdr(req, "Location", "/");
        return httpd_resp_send(req, NULL, 0);
    }
    if (req->content_len <= 0 || req->content_len > BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
    }
    char *body = calloc(1, BODY_MAX + 1);
    if (!body) {
        return httpd_resp_send_500(req);
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    char sta_ssid[HUB_SSID_MAX * 3 + 1], sta_pass[HUB_PASS_MAX * 3 + 1];
    char ap_ssid[HUB_SSID_MAX * 3 + 1], appw[HUB_PASS_MAX * 3 + 1];
    char pw[HUB_PASS_MAX * 3 + 1], pw2[HUB_PASS_MAX * 3 + 1];
    form_get(body, "sta_ssid", sta_ssid, sizeof(sta_ssid));
    form_get(body, "sta_pass", sta_pass, sizeof(sta_pass));
    form_get(body, "ap_ssid", ap_ssid, sizeof(ap_ssid));
    form_get(body, "appw", appw, sizeof(appw));
    form_get(body, "pw", pw, sizeof(pw));
    form_get(body, "pw2", pw2, sizeof(pw2));
    free(body);

    char err[96] = "";
    esp_err_t r;
    if (strcmp(pw, pw2) != 0) {
        r = ESP_ERR_INVALID_ARG;
        strlcpy(err, "관리자 암호 확인이 일치하지 않습니다", sizeof(err));
    } else {
        r = hub_state_provision(s_st, sta_ssid, sta_pass, ap_ssid, appw, pw, err, sizeof(err));
    }
    memset(pw, 0, sizeof(pw));
    memset(pw2, 0, sizeof(pw2));
    if (r != ESP_OK) {
        return setup_form(req, err);
    }

    char b[768] = "<h1>설정 완료</h1><div class=card><p>허브가 집 Wi-Fi <b>";
    cat_esc(b, sizeof(b), s_st->sta_ssid);
    strlcat(b, "</b>에 연결하고, 전용 네트워크 <b>", sizeof(b));
    cat_esc(b, sizeof(b), s_st->ap_ssid);
    strlcat(b, "</b>로 바뀝니다.</p><p class=muted>잠시 후 이 설정용 네트워크는 닫힙니다. "
               "폰은 원래 쓰던 Wi-Fi 로 돌아가면 됩니다.</p></div>", sizeof(b));
    send_page(req, "설정 완료", b, NULL);
    xTaskCreate(apply_task, "apply_wifi", 3072, NULL, 5, NULL);
    return ESP_OK;
}

/* ---------- 관리 화면: 로그인 세션 ---------- */
#define SESS_SEC 1800
static char s_sess[33];
static int64_t s_sess_until;

static bool authed(httpd_req_t *req)
{
    char ck[160];
    if (!s_sess[0] || esp_timer_get_time() > s_sess_until ||
        httpd_req_get_hdr_value_str(req, "Cookie", ck, sizeof(ck)) != ESP_OK) {
        return false;
    }
    char *p = strstr(ck, "lynnai_s=");
    if (!p || strlen(p + 9) < 32) {
        return false;
    }
    uint8_t d = 0;
    for (int i = 0; i < 32; i++) {                   /* 같은 시간에 비교 */
        d |= (uint8_t)(p[9 + i] ^ s_sess[i]);
    }
    if (d) {
        return false;
    }
    s_sess_until = esp_timer_get_time() + (int64_t)SESS_SEC * 1000000;
    return true;
}

bool hub_portal_authed(httpd_req_t *req) { return authed(req); }

static int read_body(httpd_req_t *req, char *body, int cap)
{
    if (req->content_len <= 0 || req->content_len >= cap) {
        return -1;
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    body[got] = 0;
    return got;
}

static esp_err_t redirect(httpd_req_t *req, const char *to)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", to);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t login_page(httpd_req_t *req, const char *msg)
{
    char b[640] = "<h1>관리 화면</h1>";
    if (msg) {
        strlcat(b, "<p class=warn>", sizeof(b)); cat_esc(b, sizeof(b), msg); strlcat(b, "</p>", sizeof(b));
    }
    strlcat(b, "<div class=card><form method=post action=/login><label>관리자 암호</label>"
               "<input name=pw type=password autofocus><button>로그인</button></form>"
               "<p class=muted>처음 설정할 때 정한 관리자 암호입니다.</p></div>", sizeof(b));
    return send_page(req, "관리 화면", b, NULL);
}

/* 관리 화면 공통: 별명 팝업 + 변경 추적(바뀐 줄 옅은 테두리, 저장 버튼은 바뀐 게 있을 때만 강조) + 버튼을 새로 열지 않고 보내기.
 * 저장 버튼은 class=sv 를 단다. 별명 칩은 editAlias('<mac>') 를 부르고, 팝업 [저장]은 숨은 a_<mac> 에 값을 넣고
 * 그 폼을 바로 제출한다(별도 저장 버튼을 또 누를 필요 없음). */
static const char ADMIN_EXTRA[] =
    "<dialog id=adlg><h3 style='margin-top:0'>이름</h3><p class=muted style='margin:0 0 8px'>비우고 저장하면 기기를 삭제합니다</p>"
    "<input id=adin maxlength=20 placeholder='예: 거실 보일러'>"
    "<div class=dbtn><button type=button id=adok>저장</button><button type=button id=adno class=ghost>취소</button></div>"
    "</dialog><script>(function(){"
    "document.querySelectorAll('form').forEach(function(f){var b=f.querySelector('button.sv');if(!b)return;"
    "var fs=[].slice.call(f.querySelectorAll('input:not([type=hidden]),select'));"
    "function cv(e){return e.type=='checkbox'||e.type=='radio'?e.checked:e.value}"
    "fs.forEach(function(e){e._o=cv(e)});"
    "function up(){var n=0;f.querySelectorAll('.chg').forEach(function(r){r.classList.remove('chg')});"
    "fs.forEach(function(e){var v=cv(e);"
    "if(v!==e._o){n++;(e.closest('.row')||e).classList.add('chg')}});b.classList.toggle('on',n>0)}"
    "f.addEventListener('input',up);f.addEventListener('change',up)});"
    "var D=document.getElementById('adlg'),T=document.getElementById('adin');"
    "window.editAlias=function(m){var i=document.querySelector('[name=a_'+m+']');if(!i)return;"
    "T.value=i.value;D._m=m;D.showModal();T.focus();T.select()};"
    "function ok(){var m=D._m,i=document.querySelector('[name=a_'+m+']'),v=T.value.trim(),f=i.form;"
    "if(!v){if(!confirm('이름을 비우면 이 기기를 삭제합니다. 삭제할까요?'))return;"
    "D.close();var h=document.createElement('input');h.type='hidden';h.name='del';h.value=m;f.appendChild(h);"
    "f.action='/admin/device-del';go(f);return}"
    "i.value=v;document.getElementById('al_'+m).textContent=v;D.close();go(f)}"
    "document.getElementById('adok').onclick=ok;document.getElementById('adno').onclick=function(){D.close()};"
    "T.addEventListener('keydown',function(e){if(e.key=='Enter'){e.preventDefault();ok()}});"
    /* 버튼은 전부 화면을 새로 열지 않고 보낸다: 폼을 fetch 로 보내고, 돌아온 관리 화면으로 본문만 갈아 끼운다(스크롤 유지).
     * 결과 문구는 아래쪽에 잠깐 띄운다. 로그아웃과, 자기 방식으로 보내는 폼(onsubmit 이 막은 것)은 그대로 둔다.
     * 본문을 갈아 끼우면 이 스크립트도 다시 돌므로, 문서에 거는 것은 한 번만(_ajx). */
    "function go(f){f.requestSubmit?f.requestSubmit():f.submit()}"
    "function toast(t,k){var e=document.createElement('div');e.textContent=t;e.setAttribute('role','status');"
    "e.style.cssText='position:fixed;left:50%;bottom:24px;transform:translateX(-50%);max-width:88%;padding:10px 16px;"
    "border-radius:10px;color:#fff;z-index:9;box-shadow:0 4px 16px #000a;background:'+(k?'#1d5c2e':'#7a1f1f');"
    "document.body.appendChild(e);setTimeout(function(){e.remove()},4500)}"
    "var am=document.getElementById('amsg');if(am&&window._ajx>1)toast(am.textContent,am.className=='ok');"
    "if(!window._ajx){window._ajx=1;var lb;"
    "document.addEventListener('click',function(e){var b=e.target.closest&&e.target.closest('button');if(b)lb=b},true);"
    "document.addEventListener('submit',function(e){var f=e.target,a=f.getAttribute('action')||'';"
    "if(e.defaultPrevented||(f.method||'').toLowerCase()!='post'||a.indexOf('/admin')!=0)return;"
    "e.preventDefault();var b=e.submitter||(lb&&lb.form===f?lb:null),d=new URLSearchParams(new FormData(f));"
    "if(b&&b.name)d.append(b.name,b.value);if(b){b.disabled=true;b.style.opacity=.5}"
    "var y=window.scrollY;"
    "fetch(a,{method:'POST',credentials:'same-origin',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:d.toString()})"
    ".then(function(r){return r.text()}).then(function(t){"
    "var n=new DOMParser().parseFromString(t,'text/html');"
    "if(!n.getElementById('adlg')){document.open();document.write(t);document.close();return}"   /* 로그인 화면 등: 그대로 보여 준다 */
    "window._ajx=2;document.body.innerHTML=n.body.innerHTML;"
    "document.body.querySelectorAll('script').forEach(function(o){var s=document.createElement('script');"
    "s.textContent=o.textContent;o.replaceWith(s)});window.scrollTo(0,y)})"
    ".catch(function(){if(b){b.disabled=false;b.style.opacity=''}toast('허브에 닿지 않습니다 — 다시 눌러 보세요',false)})})}"
    "})();</script>";

#define FORM_MAX 6144                    /* 연결된 기기 폼: 기기 6대 × 방 7개 × 통로별 숨김 + 별명(URL 인코딩 3배) */

/* 긴 페이지(관리 화면)는 청크로 보낸다: 큰 연속 블록(본문 36KB + send_page 사본 37KB)을 한 번에 안 잡아,
 * 프록시 모드로 힙이 조각나도 실패하지 않는다. 빈 문자열을 보내면 청크가 끝나 버리므로 비어 있으면 안 보낸다. */
static void page_begin(httpd_req_t *req, const char *title)
{
    common_headers(req);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr_chunk(req, HEAD1);
    httpd_resp_sendstr_chunk(req, title);
    httpd_resp_sendstr_chunk(req, HEAD2);
}
static esp_err_t page_end(httpd_req_t *req, const char *extra)
{
    if (extra && extra[0]) {
        httpd_resp_sendstr_chunk(req, extra);
    }
    httpd_resp_sendstr_chunk(req, TAIL);
    return httpd_resp_send_chunk(req, NULL, 0);      /* 끝(빈 청크 하나) */
}

static esp_err_t admin_page(httpd_req_t *req, const char *msg, bool ok)
{
    const size_t cap = 16384;           /* 섹션별 버퍼 — 섹션 하나씩 만들어 청크로 보낸다(조각화에 강함) */
    char *b = calloc(1, cap);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    page_begin(req, "허브 관리");
#define FLUSH() do { if (b[0]) { httpd_resp_sendstr_chunk(req, b); b[0] = '\0'; } } while (0)
    strlcat(b, "<h1>LYNNAI 허브 관리</h1><p class=muted>기기 ", cap);
    cat_esc(b, cap, s_st->device_id);
    strlcat(b, " · <form method=post action=/logout style='display:inline'><button class=lnk>로그아웃</button></form></p>", cap);
    if (msg) {
        strlcat(b, ok ? "<p id=amsg class=ok>" : "<p id=amsg class=warn>", cap); cat_esc(b, cap, msg); strlcat(b, "</p>", cap);
    }
    FLUSH();
    {
        char w[400];                    /* 저장 데이터 오류가 있으면 맨 위에 경고 */
        hub_store_errors_html(w, sizeof(w));
        if (w[0]) {
            httpd_resp_sendstr_chunk(req, w);
        }
    }
    hub_uplink_alert_html(b, cap); FLUSH();   /* Zigbee 가입 정보 분실 안내(있을 때만) */
    hub_proxy_render(b, cap); FLUSH();     /* 동작 모드(프록시/단독) */
    hub_uplink_render(b, cap); FLUSH();    /* 연결 방식(통로) */
    hub_pairs_render(b, cap); FLUSH();     /* 연결된 기기 */
    hub_devs_render(b, cap); FLUSH();      /* 등록 대기 */
    hub_expose_render_types(b, cap); FLUSH();  /* 노출 설정 */
    hub_plugins_render(b, cap); FLUSH();
    hub_tariff_render(b, cap); FLUSH();    /* 가스 요금(자동/수동 + 월별 그래프) */
    hub_crypt_render(b, cap); FLUSH();     /* 저장 데이터 암호화 */
    hub_ota_render(b, cap); FLUSH();       /* 시스템: 펌웨어 OTA · 재부팅 · Zigbee 상태 */
    hub_dbgkey_render(b, cap); FLUSH();    /* 원격 디버그 키 */
    strlcat(b, "<h2>서버 연동 (장기 기록·그래프)</h2><div class=card><p class=muted>"
               "<code>/api/v1/state</code> (JSON), <code>/metrics</code> (Prometheus), <code>/api/events</code>. ", cap);
    strlcat(b, hub_api_has_token() ? "API 토큰 <b>켜짐</b>: 요청에 <code>Authorization: Bearer &lt;토큰&gt;</code> 필요.</p>"
                                   : "API 토큰 <b>없음</b>: 집 LAN 에서 누구나 읽을 수 있습니다.</p>", cap);
    strlcat(b, "<form method=post action=/admin/api><button name=op value=new>토큰 새로 만들기</button>"
               "<button name=op value=off class=ghost>토큰 끄기</button></form>"
               "<p class=muted>새로 만들면 이전 토큰은 바로 무효. 토큰은 만들 때 한 번만 보여 줍니다.</p></div>", cap);
    FLUSH();
#undef FLUSH
    free(b);
    return page_end(req, ADMIN_EXTRA);
}

static esp_err_t admin_get(httpd_req_t *req)
{
    log_req(req);
    if (!s_st->provisioned) {
        return redirect(req, "/");
    }
    return authed(req) ? admin_page(req, NULL, false) : login_page(req, NULL);
}

/* 암호 맞히기 막기: 같은 주소에서 연달아 5번 틀리면 그 주소만 5분 동안 로그인 받지 않음(틀릴 때마다 1초 지연은 그대로).
 * 주소별이라 LAN 의 다른 기기가 일부러 틀려 관리자를 계속 막을 수는 없다. 표는 작게(넘치면 가장 오래된 칸을 씀). */
#define LOGIN_MAX_FAIL 5
#define LOGIN_LOCK_US  (5LL * 60 * 1000000)
#define LOGIN_SLOTS    8
static struct {
    uint32_t ip;
    int fail;
    int64_t until, last;
} s_login[LOGIN_SLOTS];

/* 주소를 바꿔 가며 틀리는 것 막기: 모든 주소의 실패를 합쳐 LOGIN_GMAX 번이면 5분 동안, 이번 부팅에 로그인에 성공한 적
 * 없는 주소는 받지 않는다(성공한 적 있는 관리자 주소는 그대로 — 남이 일부러 틀려 관리자를 막지 못하게). */
#define LOGIN_GMAX   20
#define LOGIN_GOOD   4
static int s_gfail;
static int64_t s_glast, s_glock_until;
static uint32_t s_good_ip[LOGIN_GOOD];
static int s_good_n;

static bool login_good_ip(uint32_t ip)
{
    for (int i = 0; i < LOGIN_GOOD; i++) {
        if (ip && s_good_ip[i] == ip) {
            return true;
        }
    }
    return false;
}

static uint32_t req_ip(httpd_req_t *req)
{
    struct sockaddr_storage a;
    socklen_t l = sizeof(a);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&a, &l) < 0) {
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

static int login_slot(uint32_t ip, bool make)
{
    int old = 0;
    for (int i = 0; i < LOGIN_SLOTS; i++) {
        if (s_login[i].ip == ip && s_login[i].last) {
            return i;
        }
        if (s_login[i].last < s_login[old].last) {
            old = i;
        }
    }
    if (!make) {
        return -1;
    }
    s_login[old].ip = ip;
    s_login[old].fail = 0;
    s_login[old].until = 0;
    s_login[old].last = esp_timer_get_time();
    return old;
}

static esp_err_t login_post(httpd_req_t *req)
{
    log_req(req);
    char body[256], pw[HUB_PASS_MAX * 3 + 1];
    if (!s_st->provisioned || read_body(req, body, sizeof(body)) < 0) {
        return redirect(req, "/");
    }
    uint32_t ip = req_ip(req);
    int ls = login_slot(ip, false);
    if ((ls >= 0 && esp_timer_get_time() < s_login[ls].until) ||
        (esp_timer_get_time() < s_glock_until && !login_good_ip(ip))) {
        memset(body, 0, sizeof(body));
        return login_page(req, "암호를 여러 번 틀려 5분 동안 잠겼습니다. 잠시 뒤 다시 하세요");
    }
    form_get(body, "pw", pw, sizeof(pw));
    memset(body, 0, sizeof(body));
    bool ok = pw[0] && hub_state_verify_admin(pw);
    memset(pw, 0, sizeof(pw));
    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(1000));             /* 암호 맞히기 늦추기 */
        ls = login_slot(ip, true);
        s_login[ls].last = esp_timer_get_time();
        if (s_login[ls].last - s_glast > 10LL * 60 * 1000000) {
            s_gfail = 0;                               /* 10분 넘게 조용했으면 합계는 처음부터 */
        }
        s_glast = s_login[ls].last;
        if (++s_gfail >= LOGIN_GMAX) {
            s_gfail = 0;
            s_glock_until = s_glast + LOGIN_LOCK_US;
            ESP_LOGW(TAG, "관리자 암호를 여러 주소에서 %d번 틀림 — 로그인한 적 없는 주소는 5분 잠금", LOGIN_GMAX);
        }
        if (++s_login[ls].fail >= LOGIN_MAX_FAIL) {
            s_login[ls].fail = 0;
            s_login[ls].until = esp_timer_get_time() + LOGIN_LOCK_US;
            ESP_LOGW(TAG, "관리자 암호 %d번 틀림(" IPSTR ") — 그 주소 5분 잠금", LOGIN_MAX_FAIL, IP2STR((esp_ip4_addr_t *)&ip));
            return login_page(req, "암호를 여러 번 틀려 5분 동안 잠겼습니다");
        }
        return login_page(req, "암호가 맞지 않습니다");
    }
    if (ls >= 0) {
        s_login[ls].fail = 0;
    }
    if (!login_good_ip(ip)) {
        s_good_ip[s_good_n++ % LOGIN_GOOD] = ip;
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        snprintf(s_sess + 2 * i, 3, "%02x", rnd[i]);
    }
    s_sess_until = esp_timer_get_time() + (int64_t)SESS_SEC * 1000000;
    char ck[96];
    snprintf(ck, sizeof(ck), "lynnai_s=%s; Path=/; HttpOnly; SameSite=Strict", s_sess);
    httpd_resp_set_hdr(req, "Set-Cookie", ck);
    return redirect(req, "/admin");
}

/* 로그아웃: 지금 세션 쿠키를 가진 요청(POST)만 세션을 끝낸다. 남이 링크·이미지로 GET 을 보내 관리자를
 * 쫓아내지 못하게 GET 은 아무것도 안 하고 관리 화면으로 보낸다. */
static esp_err_t logout_post(httpd_req_t *req)
{
    if (authed(req)) {
        s_sess[0] = 0;
        httpd_resp_set_hdr(req, "Set-Cookie", "lynnai_s=; Path=/; Max-Age=0");
    }
    return redirect(req, "/admin");
}

static esp_err_t logout_get(httpd_req_t *req)
{
    return redirect(req, "/admin");
}

/* 저장 후 센서만 다시 적용(재부팅 없음). 응답을 먼저 보낸 뒤 reload(최대 ~2초 걸림). */
static void reload_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));
    hub_sensors_reload();
    vTaskDelete(NULL);
}

static esp_err_t plugin_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char body[512], err[96] = "";
    if (read_body(req, body, sizeof(body)) < 0) {
        return admin_page(req, "잘못된 요청", false);
    }
    if (!hub_plugins_apply(body, err, sizeof(err))) {
        return admin_page(req, err, false);
    }
    xTaskCreate(reload_task, "sens_reload", 3072, NULL, 5, NULL);
    return admin_page(req, "저장했습니다 (센서만 다시 적용, 재부팅 없음)", true);
}

static esp_err_t pairs_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char *body = calloc(1, FORM_MAX), msg[160] = "";
    if (!body) {
        return httpd_resp_send_500(req);
    }
    bool ok = read_body(req, body, FORM_MAX) >= 0 && hub_pairs_apply(body, msg, sizeof(msg));
    free(body);
    return admin_page(req, ok ? msg : (msg[0] ? msg : "잘못된 요청"), ok);
}

static esp_err_t devicedel_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char *body = calloc(1, FORM_MAX), msg[96] = "";   /* 기기 폼 전체가 온다(이름 비워 삭제) */
    if (!body) {
        return httpd_resp_send_500(req);
    }
    bool ok = read_body(req, body, FORM_MAX) >= 0 && hub_pairs_delete(body, msg, sizeof(msg));
    free(body);
    return admin_page(req, ok ? msg : (msg[0] ? msg : "잘못된 요청"), ok);
}

static esp_err_t expose_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char *body = calloc(1, 2048), err[160] = "";
    if (!body) {
        return httpd_resp_send_500(req);
    }
    bool ok = read_body(req, body, 2048) >= 0 && hub_expose_apply(body, err, sizeof(err));
    free(body);
    return admin_page(req, ok ? "노출 설정을 저장했습니다 (바로 적용)" : (err[0] ? err : "잘못된 요청"), ok);
}

static esp_err_t dev_approve_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char body[64], msg[160] = "";
    bool ok = read_body(req, body, sizeof(body)) >= 0 && hub_devs_apply(body, msg, sizeof(msg));
    return admin_page(req, msg[0] ? msg : "잘못된 요청", ok);
}

static esp_err_t crypt_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char body[64], msg[200] = "";
    bool ok = read_body(req, body, sizeof(body)) >= 0 && hub_crypt_apply(body, msg, sizeof(msg));
    return admin_page(req, msg[0] ? msg : "잘못된 요청", ok);
}

static esp_err_t mode_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char body[64], msg[160] = "";
    bool ok = read_body(req, body, sizeof(body)) >= 0 && hub_proxy_apply(body, msg, sizeof(msg));
    return admin_page(req, msg[0] ? msg : "잘못된 요청", ok);
}

static esp_err_t gas_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char body[512], err[96] = "";
    if (read_body(req, body, sizeof(body)) < 0) {
        return admin_page(req, "잘못된 요청", false);
    }
    char op[12];
    form_get(body, "op", op, sizeof(op));
    if (strcmp(op, "refresh") == 0) {
        hub_tariff_refresh();
        return admin_page(req, "지금 다시 받는 중입니다", true);
    }
    char mode[12], region[16], billday[8], vcf[16], heat[16], rate[16], base[16];
    form_get(body, "mode", mode, sizeof(mode));
    form_get(body, "region", region, sizeof(region));
    form_get(body, "billday", billday, sizeof(billday));
    form_get(body, "vcf", vcf, sizeof(vcf));
    form_get(body, "heat", heat, sizeof(heat));
    form_get(body, "rate", rate, sizeof(rate));
    form_get(body, "base", base, sizeof(base));
    bool ok = hub_tariff_apply(strcmp(mode, "manual") == 0 ? 1 : 0, region, atoi(billday),
                               atof(vcf), atof(heat), atof(rate), atof(base), err, sizeof(err));
    return admin_page(req, ok ? "가스 요금 설정을 저장했습니다" : (err[0] ? err : "잘못된 요청"), ok);
}

static esp_err_t transport_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char body[128], msg[128] = "";
    bool ok = read_body(req, body, sizeof(body)) >= 0 && hub_uplink_apply(body, msg, sizeof(msg));
    return admin_page(req, ok ? msg : "잘못된 요청", ok);
}

static esp_err_t apitoken_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char body[64], op[8] = "";
    if (read_body(req, body, sizeof(body)) >= 0) {
        form_get(body, "op", op, sizeof(op));
    }
    if (!strcmp(op, "new")) {
        char tok[33], msg[160];
        if (!hub_api_token_new(tok)) {
            return admin_page(req, "토큰을 저장하지 못했습니다 — 이전 토큰이 그대로입니다", false);
        }
        snprintf(msg, sizeof(msg), "새 API 토큰: %s  (지금만 보입니다 — 서버 설정에 넣으세요)", tok);
        return admin_page(req, msg, true);
    }
    if (!strcmp(op, "off")) {
        if (!hub_api_token_clear()) {
            return admin_page(req, "저장하지 못했습니다 — 토큰이 그대로 켜져 있습니다", false);
        }
        return admin_page(req, "API 토큰을 껐습니다 (LAN 에 열림)", true);
    }
    return admin_page(req, "잘못된 요청", false);
}

static esp_err_t plugin_action_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req)) {
        return login_page(req, "로그인이 필요합니다");
    }
    char body[128], msg[96] = "";
    if (read_body(req, body, sizeof(body)) < 0) {
        return admin_page(req, "잘못된 요청", false);
    }
    bool ok = hub_plugins_action(body, msg, sizeof(msg));   /* 플러그인 즉시 동작 — 재부팅 없음 */
    return admin_page(req, msg[0] ? msg : (ok ? "적용했습니다" : "실패"), ok);
}

/* 펌웨어 올리기(OTA): 몸통이 .bin 원본. 응답은 평문(관리 화면 JS 가 XHR 로 보냄). 관리자 세션만. */
static esp_err_t ota_post(httpd_req_t *req)
{
    log_req(req);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (!authed(req) && !hub_dbgkey_check(req)) {    /* 관리자 세션 또는 원격 디버그 키 */
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "로그인 또는 원격 디버그 키가 필요합니다");
    }
    char msg[256] = "";
    bool reboot = false;
    int ok = hub_ota_write_req(req, msg, sizeof(msg), &reboot);
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    esp_err_t r = httpd_resp_sendstr(req, msg[0] ? msg : (ok ? "완료" : "실패"));
    if (ok && reboot) {
        hub_ota_schedule_reboot();         /* 응답을 보낸 뒤 재시작 */
    }
    return r;
}

/* 시스템: 지금은 재부팅(op=reboot)만. Zigbee 유령 가입(joined 인데 오프라인) 복구 레버. */
static esp_err_t system_post(httpd_req_t *req)
{
    log_req(req);
    if (!authed(req) && !hub_dbgkey_check(req)) {    /* 관리자 세션 또는 원격 디버그 키 */
        return login_page(req, "로그인 또는 원격 디버그 키가 필요합니다");
    }
    char body[48], op[16] = "";
    if (read_body(req, body, sizeof(body)) >= 0) {
        form_get(body, "op", op, sizeof(op));
    }
    if (!strcmp(op, "reboot")) {
        hub_ota_schedule_reboot();         /* 응답(아래 페이지)을 먼저 보낸 뒤 1.5초 있다 재시작 */
        return admin_page(req, "재부팅합니다 — 10~20초 뒤 관리 화면을 새로고침하세요. Zigbee 가 다시 가입됩니다.", true);
    }
    return admin_page(req, "잘못된 요청", false);
}

/* 가스 요금 '현재값'만 돌려준다(관리 화면이 페이지를 다시 그리지 않고 값만 바꾼다). ?do=refresh 면 다시 받기부터. */
static esp_err_t gasnow_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!authed(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "로그인이 필요합니다");
    }
    char q[32] = "", v[12] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
        httpd_query_key_value(q, "do", v, sizeof(v)) == ESP_OK && !strcmp(v, "refresh")) {
        hub_tariff_refresh();
    }
    char b[640] = "";
    hub_tariff_now_html(b, sizeof(b));
    return httpd_resp_sendstr(req, b);
}

/* 원격 디버그 키 만들기/삭제 — 키는 관리자만 만든다(암호 필요). 디버그 키로는 또 못 만든다. */
static esp_err_t dbgkey_post(httpd_req_t *req)
{
    log_req(req);
    /* 관리 화면은 fetch(X-Ajax)로 보내고 카드 안쪽만 받는다. 페이지째 응답하면 주소가 /admin/dbgkey 로 남아
     * 새로고침할 때마다 폼이 다시 보내져 키가 또 생긴다. 스크립트가 꺼진 브라우저만 예전처럼 페이지로 답한다. */
    bool ajax = httpd_req_get_hdr_value_len(req, "X-Ajax") > 0;
    if (!authed(req)) {
        if (ajax) {
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            return httpd_resp_sendstr(req, "로그인이 풀렸습니다 — 페이지를 새로고침해 다시 로그인하세요");
        }
        return login_page(req, "로그인이 필요합니다");
    }
    char body[288], op[8] = "", tok[33] = "";       /* 한글 별명은 URL 인코딩으로 길어진다 */
    const char *note = "잘못된 요청";
    bool ok = false, made = false;
    if (read_body(req, body, sizeof(body)) >= 0) {
        form_get(body, "op", op, sizeof(op));
        if (!strcmp(op, "new")) {
            char alias[24] = "";
            form_get(body, "alias", alias, sizeof(alias));
            made = ok = hub_dbgkey_create(alias, tok) != 0;
            note = ok ? "" : "키를 만들지 못했습니다 (가득 찼거나 저장 실패)";
        } else if (!strcmp(op, "del")) {
            char idxs[8] = "";
            form_get(body, "idx", idxs, sizeof(idxs));
            ok = idxs[0] && hub_dbgkey_delete(atoi(idxs)) != 0;
            note = ok ? "디버그 키를 삭제했습니다" : "삭제하지 못했습니다";
        }
    }
    if (ajax) {
        const size_t cap = 4608;
        char *b = calloc(1, cap);
        if (!b) {
            return httpd_resp_send_500(req);
        }
        hub_dbgkey_render_body(b, cap, made ? tok : NULL, note, ok);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t r = httpd_resp_sendstr(req, b);
        free(b);
        return r;
    }
    if (made) {
        char msg[120];
        snprintf(msg, sizeof(msg), "새 디버그 키: %s  (지금만 보입니다 — 복사해 두세요)", tok);
        return admin_page(req, msg, true);
    }
    return admin_page(req, note, ok);
}

/* ---------- 캡티브: 모르는 주소는 전부 설정 페이지로 ---------- */
static esp_err_t captive_get(httpd_req_t *req)
{
    log_req(req);
    httpd_resp_set_status(req, "302 Found");
    char loc[40];
    uint32_t ip = hub_wifi_ap_ip();
    const uint8_t *o = (const uint8_t *)&ip;
    snprintf(loc, sizeof(loc), "http://%u.%u.%u.%u/", o[0], o[1], o[2], o[3]);
    httpd_resp_set_hdr(req, "Location", loc);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t hub_portal_start(hub_state_t *st)
{
    s_st = st;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;          /* 폰이 연결을 여러 개 열어도 오래된 것부터 정리 */
    cfg.max_open_sockets = 5;
    cfg.max_uri_handlers = 34;            /* 기본 8개면 /api 가 늘 때 맨 마지막 와일드카드가 빠진다 */
    cfg.stack_size = 6144;
    cfg.open_fn = on_open;
    cfg.close_fn = on_close;
    httpd_handle_t srv = NULL;
    esp_err_t r = httpd_start(&srv, &cfg);
    if (r != ESP_OK) {
        return r;
    }
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    const httpd_uri_t prov = { .uri = "/provision", .method = HTTP_POST, .handler = provision_post };
    const httpd_uri_t any = { .uri = "/*", .method = HTTP_GET, .handler = captive_get };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &prov);
    const httpd_uri_t boot = { .uri = "/key/server", .method = HTTP_GET, .handler = hub_proxy_bootstrap_handler };
    httpd_register_uri_handler(srv, &boot);        /* 보일러 부트스트랩 중계(와일드카드보다 먼저) */
    const httpd_uri_t adm = { .uri = "/admin", .method = HTTP_GET, .handler = admin_get };
    const httpd_uri_t lin = { .uri = "/login", .method = HTTP_POST, .handler = login_post };
    const httpd_uri_t lout = { .uri = "/logout", .method = HTTP_GET, .handler = logout_get };
    const httpd_uri_t loutp = { .uri = "/logout", .method = HTTP_POST, .handler = logout_post };
    const httpd_uri_t plug = { .uri = "/admin/plugin", .method = HTTP_POST, .handler = plugin_post };
    const httpd_uri_t gas = { .uri = "/admin/plugin-action", .method = HTTP_POST, .handler = plugin_action_post };
    const httpd_uri_t expo = { .uri = "/admin/expose", .method = HTTP_POST, .handler = expose_post };
    const httpd_uri_t prs = { .uri = "/admin/devices", .method = HTTP_POST, .handler = pairs_post };
    const httpd_uri_t ddel = { .uri = "/admin/device-del", .method = HTTP_POST, .handler = devicedel_post };
    httpd_register_uri_handler(srv, &expo);
    httpd_register_uri_handler(srv, &prs);
    httpd_register_uri_handler(srv, &ddel);
    httpd_register_uri_handler(srv, &adm);
    httpd_register_uri_handler(srv, &lin);
    httpd_register_uri_handler(srv, &lout);
    httpd_register_uri_handler(srv, &loutp);
    httpd_register_uri_handler(srv, &plug);
    httpd_register_uri_handler(srv, &gas);
    const httpd_uri_t apit = { .uri = "/admin/api", .method = HTTP_POST, .handler = apitoken_post };
    httpd_register_uri_handler(srv, &apit);
    const httpd_uri_t trns = { .uri = "/admin/transport", .method = HTTP_POST, .handler = transport_post };
    httpd_register_uri_handler(srv, &trns);
    const httpd_uri_t mode = { .uri = "/admin/mode", .method = HTTP_POST, .handler = mode_post };
    httpd_register_uri_handler(srv, &mode);
    const httpd_uri_t gasc = { .uri = "/admin/gas", .method = HTTP_POST, .handler = gas_post };
    httpd_register_uri_handler(srv, &gasc);
    const httpd_uri_t crypt = { .uri = "/admin/crypt", .method = HTTP_POST, .handler = crypt_post };
    httpd_register_uri_handler(srv, &crypt);
    const httpd_uri_t dapp = { .uri = "/admin/dev-approve", .method = HTTP_POST, .handler = dev_approve_post };
    httpd_register_uri_handler(srv, &dapp);
    const httpd_uri_t ota = { .uri = "/admin/ota", .method = HTTP_POST, .handler = ota_post };
    httpd_register_uri_handler(srv, &ota);
    const httpd_uri_t sys = { .uri = "/admin/system", .method = HTTP_POST, .handler = system_post };
    httpd_register_uri_handler(srv, &sys);
    const httpd_uri_t dbgk = { .uri = "/admin/dbgkey", .method = HTTP_POST, .handler = dbgkey_post };
    httpd_register_uri_handler(srv, &dbgk);
    const httpd_uri_t gnow = { .uri = "/admin/gas-now", .method = HTTP_GET, .handler = gasnow_get };
    httpd_register_uri_handler(srv, &gnow);
    hub_api_register(srv);                         /* /api/v1/state, /metrics (서버 기록·그래프용) */
    hub_events_register(srv);                      /* /api/events, /api/stats (NAS 기록기용) */
    httpd_register_uri_handler(srv, &any);         /* 맨 마지막: 나머지 전부 */
    ESP_LOGI(TAG, "설정 포털 시작 (:80)");
    return ESP_OK;
}
