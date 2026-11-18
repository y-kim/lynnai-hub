#!/usr/bin/env python3
"""
portal.py -- 설정 포털 (AP 전용 바인드, 인증).  보안 경계의 중심.

원칙(배포 보안 포스처):
- AP 인터페이스(uap0, 192.168.50.1)에만 바인드. wlan0 절대 노출 금지.
- 첫 접속 때 관리자 암호 설정 강제(기본 로그인 없음).
- 세션 쿠키는 기기별 session_secret 으로 HMAC 서명. 평문 암호 저장 안 함.
- 쓰기 작업은 로그인 세션 + CSRF 토큰 필요.
- /key/server 는 '보일러용' 무인증 엔드포인트(standalone 부트스트랩) — 사람 페이지와 분리.

의존성 없음(stdlib http.server). 사람과 보일러를 한 :80 서버가 경로로 분기.
"""
import base64
import hmac
import html
import http.client
import json
import os
import sys
import time
import urllib.parse
from hashlib import sha256
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "core"))
import state as state_mod  # noqa: E402

SESSION_TTL = 3600


class NullController:
    """모드 백엔드가 안 붙었을 때(또는 테스트) 보일러 조작 스텁."""
    def status(self):
        return {"attached": False, "msg": "컨트롤러 미연결(모드 백엔드 없음)"}
    def set_temp(self, zone, temp):
        raise RuntimeError("컨트롤러 미연결")
    def set_power(self, zone, on):
        raise RuntimeError("컨트롤러 미연결")


class Portal:
    def __init__(self, st, controller=None, broker_ip="192.168.50.1"):
        self.st = st
        self.ctl = controller or NullController()
        self.broker_ip = broker_ip

    # ---- 세션 토큰(HMAC 서명) ----
    def _secret(self):
        return self.st.data.get("session_secret", "fallback").encode()

    def make_session(self):
        exp = str(int(time.time()) + SESSION_TTL)
        sig = hmac.new(self._secret(), exp.encode(), sha256).hexdigest()[:32]
        return base64.urlsafe_b64encode(("%s.%s" % (exp, sig)).encode()).decode()

    def valid_session(self, token):
        try:
            raw = base64.urlsafe_b64decode(token.encode()).decode()
            exp, sig = raw.split(".", 1)
            good = hmac.new(self._secret(), exp.encode(), sha256).hexdigest()[:32]
            if not hmac.compare_digest(sig, good):
                return False
            return int(exp) > time.time()
        except Exception:
            return False

    def csrf_token(self, session):
        return hmac.new(self._secret(), ("csrf:" + session).encode(), sha256).hexdigest()[:32]

    def check_csrf(self, session, token):
        return bool(session) and hmac.compare_digest(token or "", self.csrf_token(session))


# ---------- 보일러 부트스트랩 응답(standalone: 로컬 브로커로 유도) ----------
def bootstrap_xml(broker_ip):
    # 보일러는 여기서 받은 ip:port 로 MQTT TLS 접속. uid/pass 는 아무거나 — 로컬
    # mosquitto 가 익명 허용이라 그대로 수락됨(진짜 벤더계정 불필요).
    return (
        '<?xml version="1.0" encoding="UTF-8"?>'
        "<result><tid></tid><error>S000001</error><message>success</message>"
        "<ip>%s</ip><port><mqtt>8883</mqtt><ws>6002</ws><ssl>true</ssl></port>"
        "<uid>local</uid><pass>bG9jYWw=</pass></result>" % broker_ip
    )


# ---------- HTML ----------
# 모든 암호칸(type=password)에 칸별 '눈' 버튼을 붙인다. 누르면 그 칸만 보기/숨기기.
_EYE_OPEN = ('<svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" '
             'stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'
             '<path d="M1 12s4-7 11-7 11 7 11 7-4 7-11 7S1 12 1 12z"/><circle cx="12" cy="12" r="3"/></svg>')
_EYE_SHUT = ('<svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" '
             'stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">'
             '<path d="M17.94 17.94A10.9 10.9 0 0 1 12 19c-7 0-11-7-11-7a20.3 20.3 0 0 1 5.06-5.94"/>'
             '<path d="M9.9 4.24A10.4 10.4 0 0 1 12 4c7 0 11 7 11 7a20.4 20.4 0 0 1-3.17 4.19"/>'
             '<path d="M14.12 14.12a3 3 0 1 1-4.24-4.24"/><path d="M1 1l22 22"/></svg>')
EYE_JS = ("<script>(function(){var O='" + _EYE_OPEN + "',C='" + _EYE_SHUT + "';"
          "document.querySelectorAll('input[type=password]').forEach(function(i){"
          "var w=document.createElement('span');w.className='pwwrap';i.parentNode.insertBefore(w,i);w.appendChild(i);"
          "var b=document.createElement('button');b.type='button';b.className='eye';b.innerHTML=O;"
          "b.setAttribute('aria-label','암호 보기');"
          "b.onclick=function(){var v=i.type==='password';i.type=v?'text':'password';b.innerHTML=v?C:O;"
          "b.setAttribute('aria-label',v?'암호 숨기기':'암호 보기');i.focus();};w.appendChild(b);});})();</script>")


def page(body, title="LYNNAI HUB"):
    return ("<!doctype html><html lang=ko><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>%s</title><style>"
            "body{font-family:system-ui,sans-serif;max-width:560px;margin:0 auto;padding:16px;"
            "background:#111;color:#eee}h1{font-size:20px}label{display:block;margin:12px 0 4px;color:#bbb}"
            "input,select,button{font-size:16px;padding:10px;width:100%%;box-sizing:border-box;"
            "border-radius:8px;border:1px solid #444;background:#1c1c1c;color:#eee}"
            "button{background:#c33;border:0;margin-top:14px;font-weight:600}.card{background:#191919;"
            "border:1px solid #333;border-radius:12px;padding:16px;margin:14px 0}.muted{color:#888;font-size:13px}"
            "a{color:#6af}.warn{color:#f88}"
            ".pwwrap{position:relative;display:block}.pwwrap input{padding-right:48px}"
            "button.eye{position:absolute;right:4px;top:50%%;transform:translateY(-50%%);width:40px;height:40px;"
            "margin:0;padding:8px;background:transparent;border:0;color:#aaa;line-height:0}"
            "button.eye:focus-visible{outline:2px solid #6af}"
            "</style></head><body>%s%s</body></html>"
            % (title, body, EYE_JS))


class Handler(BaseHTTPRequestHandler):
    portal = None  # 주입

    def log_message(self, fmt, *a):
        pass  # 기본 접근 로그는 끔 — 대신 log_request 에서 한 줄 요약(암호·쿠키 안 남김)

    def log_request(self, code="-", size="-"):
        ua = (self.headers.get("User-Agent") or "-") if self.headers else "-"
        print("[req] %s %s %s%s -> %s  ua=%s" % (
            self.client_address[0], self.command, self.headers.get("Host", "-") if self.headers else "-",
            urllib.parse.urlparse(self.path).path, code, ua[:60]), flush=True)

    # ---- 유틸 ----
    def _cookies(self):
        c = {}
        for part in (self.headers.get("Cookie") or "").split(";"):
            if "=" in part:
                k, v = part.strip().split("=", 1)
                c[k] = v
        return c

    def _session(self):
        tok = self._cookies().get("sid", "")
        return tok if self.portal.valid_session(tok) else None

    def _send(self, code, body, ctype="text/html; charset=utf-8", cookie=None):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Cache-Control", "no-store")
        if cookie:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(data)

    def _redirect(self, loc, cookie=None):
        self.send_response(303)
        self.send_header("Location", loc)
        if cookie:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n).decode() if n else ""
        return dict(urllib.parse.parse_qsl(raw))

    # ---- GET ----
    def do_GET(self):
        p = urllib.parse.urlparse(self.path).path
        P = self.portal
        # 보일러 부트스트랩 (무인증) — 모드별 분기
        if p == "/key/server":
            return self._bootstrap()
        if p in ("/", "/index.html"):
            return self._home()
        if p == "/logout":
            return self._redirect("/", cookie="sid=; Max-Age=0; Path=/")
        # 그 외 모든 GET = OS 캡티브 감지 프로브 / 임의 호스트 → 설정 페이지로 유도
        return self._captive()

    # OS 캡티브 포털 감지 프로브(참고). 실제론 "미지의 모든 GET"을 포털로 돌린다:
    #   Android/Chrome: /generate_204 /gen_204   Apple: /hotspot-detect.html
    #   Windows: /connecttest.txt /ncsi.txt      Firefox: /success.txt  Ubuntu: /canonical.html
    def _captive(self):
        """설정 AP에 접속하면 단말 OS가 인터넷 확인용으로 아무 주소나 때린다.
        DNS(dns_hijack)가 그 요청을 전부 이 서버로 보내고, 여기서 포털 루트로 302 →
        OS가 '네트워크 로그인' 웹뷰를 자동으로 띄워 설정 페이지가 바로 뜬다."""
        host_ip, port = self.server.server_address[:2]
        loc = "http://%s%s/" % (host_ip, "" if port == 80 else ":%d" % port)
        self.send_response(302)
        self.send_header("Location", loc)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _bootstrap(self):
        """보일러의 GET /key/server. standalone=로컬 브로커 안내, proxy=실클라우드 중계."""
        P = self.portal
        mode = P.st.data.get("mode", "standalone")
        if mode == "proxy":
            px = P.st.get_proxy()
            ip = px.get("upstream_ip") or ""
            if ip:
                try:
                    extra = {"Host": px.get("upstream_host", "www.rkiot.net")}
                    for k, v in self.headers.items():
                        if k.lower().startswith("ck-"):
                            extra[k] = v
                    conn = http.client.HTTPConnection(ip, 80, timeout=8)
                    conn.request("GET", "/key/server", headers=extra)
                    r = conn.getresponse()
                    body = r.read()
                    conn.close()
                    return self._send(r.status, body, "application/xml; charset=utf-8")
                except Exception:
                    pass  # 중계 실패 → 로컬 폴백
        return self._send(200, bootstrap_xml(P.broker_ip),
                          "application/xml; charset=utf-8")

    def _home(self):
        P = self.portal
        st = P.st
        # 1) 미프로비저닝 → 공개 setup AP 상태. 생산 SSID+암호+관리자 한 번에 설정.
        if not st.is_provisioned():
            body = ("<h1>초기 설정</h1><div class=card>"
                    "<p>이 허브를 처음 설정합니다. 아래를 정하면 <b>전용 네트워크</b>로 바뀌고,"
                    " 그 네트워크로 <b>다시 연결</b>하면 됩니다.</p>"
                    "<form method=post action=/provision>"
                    "<label>전용 네트워크 이름 (SSID)</label><input name=ssid value=\"%s\" maxlength=32>"
                    "<label>Wi-Fi 암호 (8자 이상)</label>"
                    "<input name=appw type=password class=pw id=appw oninput=_chk()>"
                    "<label>관리자 암호 (8자 이상)</label><input name=pw type=password class=pw id=pw oninput=_chk()>"
                    "<label>관리자 암호 확인</label><input name=pw2 type=password class=pw id=pw2 oninput=_chk()>"
                    "<div id=pwmsg class=muted></div>"
                    "<button id=pwbtn disabled>설정하고 전용 네트워크로 전환</button></form>"
                    "<script>function _chk(){var w=document.getElementById('appw').value,a=document.getElementById('pw').value,b=document.getElementById('pw2').value,m=document.getElementById('pwmsg'),x=document.getElementById('pwbtn');if(w.length<8){m.textContent='Wi-Fi 암호는 8자 이상';m.className='muted';x.disabled=true;}else if(a.length<8){m.textContent='관리자 암호는 8자 이상';m.className='muted';x.disabled=true;}else if(a!==b){m.textContent='암호 확인 불일치';m.className='warn';x.disabled=true;}else{m.textContent='일치합니다 ✓';m.className='muted';x.disabled=false;}}</script>"
                    "<p class=muted>지금은 공개 설정용 네트워크입니다. 설정을 마치면 닫힙니다.</p></div>"
                    % html.escape(st.data.get("ap_ssid_default") or st.data.get("device_id", "")))
            return self._send(200, page(body, "초기 설정"))
        # 2) 로그인 필요
        sid = self._session()
        if not sid:
            body = ("<h1>로그인</h1><div class=card><form method=post action=/login>"
                    "<label>관리자 암호</label><input name=pw type=password class=pw autofocus>"
                    "<button>로그인</button></form></div>")
            return self._send(200, page(body, "로그인"))
        # 3) 대시보드
        return self._send(200, page(self._dashboard(sid)))

    def _dashboard(self, sid):
        st = self.portal.st
        csrf = self.portal.csrf_token(sid)
        d = st.data
        apnote = ("<p class=muted>이름·암호를 바꾸면 전용 네트워크가 재시작되어 "
                  "<b>다시 연결</b>해야 합니다.</p>")
        ctl = self.portal.ctl.status()
        return (
            "<h1>%s</h1><p class=muted>기기 %s · 모드 <b>%s</b></p>"
            "<div class=card><h3>보일러</h3><pre>%s</pre></div>"
            "<div class=card><h3>전용 네트워크 (Wi-Fi)</h3>%s"
            "<form method=post action=/ap-ssid><input type=hidden name=csrf value=%s>"
            "<label>네트워크 이름 (SSID)</label><input name=ssid value=\"%s\" maxlength=32>"
            "<button>이름 변경</button></form>"
            "<form method=post action=/ap-pass style=margin-top:10px>"
            "<input type=hidden name=csrf value=%s>"
            "<label>새 Wi-Fi 암호 (8자 이상)</label><input name=pw type=password class=pw>"
            "<button>암호 변경</button></form></div>"
            "<div class=card><h3>동작 모드</h3><form method=post action=/mode>"
            "<input type=hidden name=csrf value=%s>"
            "<select name=mode>%s</select><button>적용</button></form>"
            "<p class=muted>standalone=클라우드 없이 자체 제어(기본) / proxy=클라우드 경유(옵트인) / bus=버스직결(준비중)</p></div>"
            "%s"  # proxy 카드(모드 proxy일 때만)
            "<div class=card><h3>공장초기화</h3><form method=post action=/factory-reset "
            "onsubmit=\"return confirm('모든 설정·비밀 삭제. 계속?')\">"
            "<input type=hidden name=csrf value=%s>"
            "<button>초기화(양도 시)</button></form></div>"
            "<p class=muted><a href=/logout>로그아웃</a></p>"
            % (html.escape(d["ap_ssid"] or ""), html.escape(d["device_id"]), html.escape(d["mode"]),
               html.escape(json.dumps(ctl, ensure_ascii=False, indent=1)),
               apnote, csrf, html.escape(d["ap_ssid"] or ""), csrf, csrf,
               "".join("<option%s>%s</option>" % (" selected" if m == d["mode"] else "", m)
                       for m in ("standalone", "proxy", "bus")),
               self._proxy_card(sid) if d["mode"] == "proxy" else "",
               csrf))

    def _proxy_card(self, sid):
        """proxy 모드 전용: 앱 키/자격증명 입력칸. 비밀은 '설정됨'만 표시, 값은 안 보냄."""
        st = self.portal.st
        csrf = self.portal.csrf_token(sid)
        view = st.proxy_view()
        rows = []
        labels = {
            "upstream_host": "실클라우드 호스트", "upstream_ip": "실클라우드 IP(하이재킹 우회)",
            "upstream_sni": "TLS SNI", "app_key": "앱 API 키", "app_secret": "앱 시크릿",
            "account_user": "린나이 계정 ID", "account_pass": "린나이 계정 암호",
            "mqtt_uid": "MQTT uid(비우면 자동)", "mqtt_pass": "MQTT pass",
        }
        for k, info in view.items():
            lab = html.escape(labels.get(k, k))
            if info["secret"]:
                ph = "(설정됨 — 바꿀 때만 입력)" if info["set"] else "(미설정)"
                rows.append("<label>%s</label><input name=%s type=password placeholder=\"%s\">"
                            % (lab, k, ph))
            else:
                val = html.escape(info["value"])
                rows.append("<label>%s</label><input name=%s value=\"%s\">" % (lab, k, val))
        return ("<div class=card><h3>proxy 전용 — 앱 키/자격증명</h3>"
                "<p class=muted>여기 아래에 값을 넣으면 <b>앱에서도</b> 보일러가 동작합니다. "
                "(standalone 모드엔 불필요) 비밀칸을 비워두면 기존값 유지.</p>"
                "<form method=post action=/proxy-config>"
                "<input type=hidden name=csrf value=%s>%s"
                "<button>저장</button></form></div>" % (csrf, "".join(rows)))

    # ---- POST ----
    def do_POST(self):
        p = urllib.parse.urlparse(self.path).path
        P = self.portal
        f = self._body()
        # provision: 미프로비저닝일 때만 (생산 SSID+암호+관리자 한 번에)
        if p == "/provision":
            if P.st.is_provisioned():
                return self._redirect("/")
            if f.get("pw") != f.get("pw2"):
                return self._send(200, page("<h1>불일치</h1><p>관리자 암호가 다릅니다. <a href=/>뒤로</a></p>"))
            try:
                appw = f.get("appw", "").strip()
                if len(appw) < 8:       # 완료 화면에 암호를 안 보여주므로 자동 생성 대신 필수
                    raise ValueError("Wi-Fi 암호는 8자 이상")
                genpw = P.st.provision(f.get("pw", ""), f.get("ssid", ""), appw)
            except (ValueError, RuntimeError) as e:
                return self._send(200, page("<h1>오류</h1><p>%s <a href=/>뒤로</a></p>" % html.escape(str(e))))
            ssid = html.escape(P.st.data["ap_ssid"])
            body = ("<h1>설정 완료</h1><div class=card>"
                    "<p>전용 네트워크로 전환합니다. <b>방금 설정한 Wi-Fi 암호</b>로 <b>다시 연결</b>하세요.</p>"
                    "<p>네트워크: <b>%s</b></p>"
                    "<p class=muted>잠시 후 이 설정용 네트워크는 닫히고 전용 네트워크가 뜹니다. 전용 네트워크에 연결한 뒤 http://192.168.50.1 에서 관리자 암호로 로그인하세요.</p></div>"
                    % ssid)
            # 세션 쿠키는 주지 않는다: 설정용 망에선 초기 설정 한 번이 끝. 관리는 전용망에서
            # 관리자 암호로 로그인해야만 가능(설정망 = 관리 권한 없음).
            self._send(200, page(body, "설정 완료"))
            self._apply_ap(delay=2.0)   # 응답 전송 후 hostapd를 생산 AP로 전환
            return
        if p == "/login":
            if P.st.verify_admin(f.get("pw", "")):
                return self._redirect("/", cookie=self._mkcookie(P.make_session()))
            return self._send(401, page("<h1>실패</h1><p>암호 틀림. <a href=/>뒤로</a></p>"))

        # 이하 세션 + CSRF 필요
        sid = self._session()
        if not sid:
            return self._send(403, page("<h1>로그인 필요</h1>"))
        if not P.check_csrf(sid, f.get("csrf")):
            return self._send(403, page("<h1>CSRF 실패</h1>"))

        if p == "/ap-pass":
            try:
                P.st.set_ap_pass(f.get("pw", ""))
            except (ValueError, RuntimeError) as e:
                return self._send(200, page("<h1>오류</h1><p>%s <a href=/>뒤로</a></p>" % html.escape(str(e))))
            self._send(200, page("<h1>암호 변경됨</h1><div class=card>"
                                 "<p>전용 네트워크가 재시작됩니다. 새 암호로 <b>다시 연결</b>하세요.</p></div>"))
            self._apply_ap(delay=2.0)
            return
        if p == "/ap-ssid":
            try:
                P.st.set_ap_ssid(f.get("ssid", ""))
            except (ValueError, RuntimeError) as e:
                return self._send(200, page("<h1>오류</h1><p>%s <a href=/>뒤로</a></p>" % html.escape(str(e))))
            self._send(200, page("<h1>이름 변경됨</h1><div class=card>"
                                 "<p>전용 네트워크가 재시작됩니다. 새 이름으로 <b>다시 연결</b>하세요.</p></div>"))
            self._apply_ap(delay=2.0)
            return
        if p == "/mode":
            try:
                P.st.set_mode(f.get("mode", "standalone"))
            except ValueError:
                pass
            return self._redirect("/")
        if p == "/proxy-config":
            # 폼에 온 proxy 필드만 반영(빈 비밀칸은 state 쪽에서 보존)
            import state as _sm
            fields = {k: f.get(k, "") for k in _sm.PROXY_FIELDS if k in f}
            P.st.set_proxy(fields)
            return self._redirect("/")
        if p == "/factory-reset":
            P.st.factory_reset()
            self._send(200, page("<h1>초기화됨</h1><div class=card><p>모든 설정·비밀을 지웠습니다. "
                                 "잠시 후 <b>새 기기 ID</b>로 설정용 네트워크(LYNNAI-SETUP-…)가 다시 뜹니다.</p></div>"))
            # 응답을 보낸 뒤 종료 → 서비스 매니저(systemd Restart=always)가 다시 띄우며 새로 첫 부팅
            import threading
            threading.Timer(2.0, os._exit, (0,)).start()
            return
        return self._send(404, page("<h1>404</h1>"))

    def _mkcookie(self, sid):
        # AP(평문 HTTP)라 Secure 불가 — HttpOnly+SameSite 로 최대한
        return "sid=%s; Max-Age=%d; Path=/; HttpOnly; SameSite=Strict" % (sid, SESSION_TTL)

    def _apply_ap(self, delay=0.0):
        """SSID/암호/프로비저닝 변경 후 hostapd를 state의 current_ap()로 재적용.
        응답을 먼저 보낸 뒤(지연) 실행해야 클라가 페이지를 받고 끊긴다.
        실배포에선 portal.on_apply_ap(ssid, pass) 훅을 모드 백엔드가 구현. 테스트선 무동작."""
        hook = getattr(self.portal, "on_apply_ap", None)
        if not hook:
            return
        ssid, pw = self.portal.st.current_ap()
        import threading
        threading.Timer(delay, lambda: hook(ssid, pw)).start()


def serve(bind_ip, port, state_path, controller=None, broker_ip=None, apply_cmd=None):
    st = state_mod.DeviceState(state_path).load()
    P = Portal(st, controller=controller, broker_ip=broker_ip or bind_ip)
    if apply_cmd:
        import subprocess
        def _apply_hook(ssid, pw):
            try:
                subprocess.Popen([apply_cmd, ssid, pw or ""])
            except Exception as e:
                print("[portal] apply-cmd 실패:", e, flush=True)
        P.on_apply_ap = _apply_hook
    Handler.portal = P
    httpd = ThreadingHTTPServer((bind_ip, port), Handler)
    print("[portal] http://%s:%d (AP 전용 바인드)" % (bind_ip, port), flush=True)
    httpd.serve_forever()


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--state", required=True)
    ap.add_argument("--apply-cmd", help="AP 적용 스크립트: <cmd> <ssid> <pass> 로 호출(프로비저닝/AP 변경 시)")
    a = ap.parse_args()
    serve(a.bind, a.port, a.state, apply_cmd=a.apply_cmd)
