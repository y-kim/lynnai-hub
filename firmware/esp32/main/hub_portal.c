/*
 * hub_portal.c -- 설정 포털(esp_http_server, :80). Pi 판 appliance/portal/portal.py 를 옮긴 것.
 *
 *  - 미설정: 첫 화면 = 설정 폼. 모르는 주소는 전부 설정 페이지로 302(캡티브: 폰이 접속하면 자동으로 뜸).
 *  - 설정 완료 응답 후 2초 뒤 Wi-Fi 를 다시 적용(설정 AP 닫고 집 Wi-Fi + 전용 AP), DNS 는 클라우드 이름만.
 *  - 설정 완료 때 세션을 주지 않는다: 설정용 망에선 초기 설정 한 번이 끝. 관리 화면은 다음 단계.
 *  - 완료 화면에 암호를 보여주지 않는다(직접 입력한 값이므로).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hub_dns.h"
#include "hub_portal.h"
#include "hub_wifi.h"
#include "hub_events.h"
#include "hub_sensors.h"
#include "hub_proxy.h"
#include "hub_zigbee.h"

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
    "input,button{font-size:16px;padding:10px;width:100%;box-sizing:border-box;border-radius:8px;"
    "border:1px solid #444;background:#1c1c1c;color:#eee}"
    "button{background:#c33;border:0;margin-top:14px;font-weight:600}button:disabled{opacity:.45}"
    ".card{background:#191919;border:1px solid #333;border-radius:12px;padding:16px;margin:14px 0}"
    ".muted{color:#888;font-size:13px}.warn{color:#f88}"
    ".pwwrap{position:relative;display:block}.pwwrap input{padding-right:48px}"
    "button.eye{position:absolute;right:4px;top:50%;transform:translateY(-50%);width:40px;height:40px;"
    "margin:0;padding:8px;background:transparent;border:0;color:#aaa;line-height:0}"
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
    strlcat(b, " · 설정 완료</p><p class=muted>관리 화면은 다음 단계에서 추가됩니다.</p></div>", sizeof(b));
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
    cfg.max_uri_handlers = 16;            /* 기본 8개면 /api 가 늘 때 맨 마지막 와일드카드가 빠진다 */
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
    hub_sensors_register(srv);                     /* /api/gas, /api/gas/set */
    hub_events_register(srv);                      /* /api/events, /api/stats (NAS 기록기용) */
    httpd_register_uri_handler(srv, &any);         /* 맨 마지막: 나머지 전부 */
    ESP_LOGI(TAG, "설정 포털 시작 (:80)");
    return ESP_OK;
}
