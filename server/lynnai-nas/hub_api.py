"""
hub_api.py (대응 ESP32: hub_api.c) -- 서버 인터페이스(장기 기록·그래프). ESP32 펌웨어 hub_api.c 와 같은 형식이라
lynnai-hub-watch·Prometheus 가 NAS 든 ESP32 든 똑같이 읽는다.

  GET /api/v1/state   지금 상태(JSON): 보일러(해독+raw), 각방, 연결, time
  GET /metrics        Prometheus 텍스트(lynnai_*)
  GET /api/stats      부팅 id·up(watch 호환)   GET /api/events  (NAS 는 이벤트 링이 없어 빈 목록)
토큰(API_TOKEN) 있으면 Authorization: Bearer 필요. 16자 미만이면 약해서 /api·/metrics 를 막는다(경고 로그).

GET /key/server(기기 부트스트랩, 인증 없음) — ESP32 hub_proxy_bootstrap_handler 와 같은 규칙:
  proxy      : 클라우드에 다시 묻고, 정상(200 + 성공 XML)이면 (서비스, MAC)별로 DATA_DIR/boot/ 에 저장해 그대로.
               클라우드 오류(2xx 가 아닌 상태)는 그 상태·본문 그대로(저장 안 함), 응답이 없으면 이 기기의 저장분, 없으면 502.
               2xx 인데 성공 XML 형식이 아니거나, <ip> 가 rkiot.net 이름이 아니거나, MQTT 포트가 다르면(변조) 버린다
               — 응답 없음과 같게(기기에 전달하지 않는다).
  standalone : 허브가 만든 답만(uid/pass 는 자리 표시 — 단독 브로커는 무엇이든 받는다). 저장한 클라우드 계정은 안 내준다.
  등록된 기기(RINNAI_IPS·승인한 기기)의 요청만 받는다(나머지 403). RINNAI_IPS=any 면 누구에게나.
  등록 안 된 출발지가 Ck-Mac 을 달고 오면 후보로 적는다(답은 그대로 403).

기기 등록(hub_devices 의 후보 → 승인). 암호화 없는 HTTP 다 — NAS 의 다른 API 와 같이 집 LAN 안에서만.
  GET  /api/v1/devices            {registered, static, candidates, pairing_until} — 다른 /api 와 같은 토큰 규칙
  POST /api/v1/devices/approve    본문(JSON 또는 폼) ip= 또는 mac= (+note=) → 후보를 등록부로
  POST /api/v1/devices/remove     본문 mac= 또는 ip= → 등록부에서 삭제
  GET  /devices                   브라우저용 페이지. 폼(토큰을 폼 값으로)을 /devices·/devices/approve·/devices/remove 로 보낸다
  승인·삭제는 API_TOKEN 이 있어야만 받는다(없으면 403 — 토큰 없이 받으면 LAN 의 아무 기기나 자기를 승인할 수 있다).

:80 은 동시 연결 HTTP_MAX_CONN 개(출발지당 HTTP_MAX_PER_IP, 등록된 기기는 따로 센다), 요청 하나를 HTTP_READ_SEC 초 안에
다 보내지 않으면 끊는다 — 유휴 연결을 쌓아 스레드·파일 번호를 다 쓰지 못하게.
"""
import hmac
import html
import io
import json
import os
import re
import random
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

import hub_devices
import hub_proxy                     # 등록된 기기 판정(RINNAI_IPS·등록부)·연결 수 세기를 :8883 과 같이 쓴다

KIND_F06, KIND_F09 = 6, 9
DATA = os.environ.get("DATA_DIR", "/data")
STANDALONE = os.environ.get("MODE", "proxy") == "standalone"
HTTP_MAX_CONN = 32                   # :80 동시 연결 상한(등록 안 된 출발지끼리 — hub_proxy.Slots)
HTTP_MAX_PER_IP = 8                  # 한 출발지당(기록기·Prometheus·브라우저가 같은 주소에서 와도 넉넉하게)
HTTP_READ_SEC = 10                   # 요청(요청 줄 + 헤더 + 본문)을 다 받는 기한. 응답 쓰기 한 번의 제한시간도 이 값
MAX_BODY = 4096                      # POST 본문 상한(기기 승인·삭제 폼은 수십 바이트)
BOOT_LOCAL = (b'<?xml version="1.0" encoding="UTF-8"?><result><tid></tid><error>S000001</error><message>success</message>'
              b'<ip>www.rkiot.net</ip><port><mqtt>8883</mqtt><ws>6002</ws><ssl>true</ssl></port>'
              b'<uid>lynnai</uid><pass>standalone</pass></result>')


def _boot_key(svc, mac):
    """ESP32 와 같은 키: 'bt' + (서비스|MAC) FNV-1a 32비트"""
    h = 2166136261
    for c in (svc + "|" + mac).encode():
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return "bt%08x" % h


def _boot_valid(body):
    return all(t in body for t in (b"<result>", b"</result>", b"<error>S000001</error>", b"<uid>", b"<pass>", b"<ip>"))


BOOT_HOST = os.environ.get("UPSTREAM_SNI", "www.rkiot.net").lower()
BOOT_DOMAIN = "rkiot.net"            # 우리 DNS 가 가로채는 이름(HIJACK_NAMES 기본) — 기기가 이 이름으로 와야 우리에게 온다
BOOT_MQTT = os.environ.get("MQTT_PORT", "8883")


class _DeadlineReader(io.RawIOBase):
    """요청을 기한 안에 다 받게 하는 읽기 쪽. 소켓 제한시간만으로는 한 바이트씩 흘려 보내는 연결을 못 끊는다."""
    def __init__(self, sock, sec):
        self.sock, self.sec, self.end = sock, sec, time.monotonic() + sec

    def readable(self):
        return True

    def readinto(self, b):
        left = self.end - time.monotonic()
        if left <= 0:
            raise socket.timeout("요청 기한 초과")
        self.sock.settimeout(left)
        try:
            return self.sock.recv_into(b)
        finally:
            self.sock.settimeout(self.sec)        # 응답 쓰기는 원래 제한시간으로


def _boot_sane(body):
    """성공 응답의 접속 정보가 우리에게 오는 값인가: <ip> 는 rkiot.net(또는 그 하위 이름)·UPSTREAM_SNI, <mqtt> 는 우리 포트.
    평문 HTTP 로 받으므로 중간에서 바꾼 <ip>(다른 서버로 보내기)를 저장·전달하지 않게."""
    ips = re.findall(rb"<ip>([^<]*)</ip>", body)
    if len(ips) != 1:
        return False
    host = ips[0].strip().decode("ascii", "replace").lower()
    if not (host == BOOT_HOST or host == BOOT_DOMAIN or host.endswith("." + BOOT_DOMAIN)):
        return False
    ports = re.findall(rb"<mqtt>([^<]*)</mqtt>", body)
    return all(p.strip().decode("ascii", "replace") == BOOT_MQTT for p in ports)
# 각방 슬롯(드라이버 core.lua ZONES·ESP32 hub_api.c 와 같다): (id, 이름, 상태, 현재, 설정) — z3 = 거실은 옛 배치 유지
ZONES = [("z1", "방1", 0, 3, 4), ("z2", "방2", 7, 10, 11), ("z4", "방3", 14, 17, 18), ("z5", "방4", 21, 24, 25),
         ("z6", "방5", 28, 31, 32), ("z7", "방6", 35, 38, 39), ("z3", "거실", 42, 45, 46)]


def _present(r, z):
    """온도 조절기가 리포트하는 방인가(상태·현재 온도 둘 다 0 = 그 슬롯에 방 없음)."""
    return z[4] < len(r) and (r[z[2]] or r[z[3]])
T0 = time.time()
BOOT_ID = random.getrandbits(31)


def _nib(b, i):
    return (b[i // 2] >> 4) if i % 2 == 0 else (b[i // 2] & 0xF)


def decode_boiler(b):
    if len(b) < 16:
        return None
    sw = {"power": 0, "heat": 2, "hotwater": 3, "preheat": 5, "outing": 19}
    m = {k: _nib(b, p) == 1 for k, p in sw.items()}
    m["indoor"] = _nib(b, 7) == 1
    m["hw_set"] = _nib(b, 10) * 16 + _nib(b, 11)
    m["ondol_set"] = _nib(b, 13) * 16 + _nib(b, 14)
    m["room_set"] = _nib(b, 15) * 16 + _nib(b, 16)
    return m


def _boiler_json(b):
    bo = decode_boiler(b)
    if not bo:
        return None
    return {"power": bo["power"], "heat": bo["heat"], "outing": bo["outing"],
            "hotwater": bo["hotwater"], "preheat": bo["preheat"],
            "heat_mode": "indoor" if bo["indoor"] else "ondol",
            "heat_set": bo["room_set"] if bo["indoor"] else bo["ondol_set"],
            "ondol_set": bo["ondol_set"], "room_set": bo["room_set"], "hw_set": bo["hw_set"], "raw": b.hex()}


def _rooms_json(r):
    if not r or len(r) < 50:
        return None
    return {"raw": r.hex(), "zones": [
        {"id": z[0], "name": z[1], "on": bool(r[z[2]] & 0x02), "heating": bool(r[z[2]] & 0x40),
         "outing": bool(r[z[2]] & 0x10), "reserve": bool(r[z[2]] & 0x08), "interval": r[z[2] + 1],
         "current": r[z[3]], "target": r[z[4]]} for z in ZONES if _present(r, z)]}


def build_state(core, hub_id):
    snap = core.snapshot()     # [(kind, mac_hex, blob, linked), ...]
    t = time.time()
    boilers, thermostats = [], []
    linked_b = linked_r = False
    for kind, mac, blob, linked in snap:
        if kind == KIND_F06:
            linked_b = linked_b or linked
            d = _boiler_json(blob)
            if d:
                d["mac"], d["linked"] = mac, linked
                boilers.append(d)
        elif kind == KIND_F09:
            linked_r = linked_r or linked
            d = _rooms_json(blob)
            if d:
                d["mac"], d["linked"] = mac, linked
                thermostats.append(d)
    return {"v": 1, "id": hub_id, "up_s": int(t - T0), "time": int(t),
            "links": {"boiler": linked_b, "rooms": linked_r},
            "boilers": boilers, "thermostats": thermostats,
            "boiler": boilers[0] if boilers else None,       # 하위호환(기록기)
            "rooms": thermostats[0] if thermostats else None}


def build_metrics(core, hub_id):
    snap = core.snapshot()
    boilers = [(mac, blob, linked) for k, mac, blob, linked in snap if k == KIND_F06]
    thermos = [(mac, blob, linked) for k, mac, blob, linked in snap if k == KIND_F09 and len(blob) >= 50]
    L = ["# lynnai NAS metrics",
         "# TYPE lynnai_uptime_seconds gauge", f'lynnai_uptime_seconds{{hub="{hub_id}"}} {int(time.time() - T0)}',
         "# TYPE lynnai_link_up gauge"]
    for mac, _b, linked in boilers:
        L.append(f'lynnai_link_up{{device="boiler",mac="{mac}"}} {int(linked)}')
    for mac, _b, linked in thermos:
        L.append(f'lynnai_link_up{{device="rooms",mac="{mac}"}} {int(linked)}')
    L.append("# TYPE lynnai_boiler_on gauge")
    for mac, b, _l in boilers:
        bo = decode_boiler(b)
        if bo:
            for w in ("power", "heat", "outing", "hotwater", "preheat"):
                L.append(f'lynnai_boiler_on{{mac="{mac}",what="{w}"}} {int(bo[w])}')
    L.append("# TYPE lynnai_boiler_setpoint_celsius gauge")
    for mac, b, _l in boilers:
        bo = decode_boiler(b)
        if bo:
            mode = "indoor" if bo["indoor"] else "ondol"
            L.append(f'lynnai_boiler_setpoint_celsius{{mac="{mac}",what="heat",mode="{mode}"}} {bo["room_set"] if bo["indoor"] else bo["ondol_set"]}')
            L.append(f'lynnai_boiler_setpoint_celsius{{mac="{mac}",what="hotwater"}} {bo["hw_set"]}')
    for metric, idx, mask in (("temperature_celsius", 3, None), ("setpoint_celsius", 4, None),
                              ("on", 2, 0x02), ("heating", 2, 0x40)):
        L.append(f"# TYPE lynnai_zone_{metric} gauge")
        for mac, r, _l in thermos:
            for z in ZONES:
                if not _present(r, z):
                    continue
                v = (int(bool(r[z[idx]] & mask)) if mask else r[z[idx]])
                L.append(f'lynnai_zone_{metric}{{mac="{mac}",zone="{z[0]}",name="{z[1]}"}} {v}')
    return "\n".join(L) + "\n"


MIN_TOKEN = 16
NO_TOKEN_MSG = ("API_TOKEN 이 없어 기기 승인·삭제를 받지 않음 — .env 에 API_TOKEN(16자 이상)을 넣고 다시 시작할 것"
                "(토큰 없이 받으면 집 LAN 의 아무 기기나 자기를 승인할 수 있다)")
_CHANGE_STATUS = {"ok": 200, "exists": 200, "notfound": 404, "full": 409, "bad": 400, "savefail": 500}


def devices_view():
    """기기 등록 상태: 등록부(승인한 기기)·고정(RINNAI_IPS)·후보·등록 창."""
    reg = hub_proxy.REGISTRY
    v = reg.snapshot() if reg is not None else {"registered": [], "candidates": [], "pairing_until": 0}
    return {"registered": v["registered"],
            "static": ["any"] if hub_proxy.DEVICE_ANY else sorted(hub_proxy.DEVICE_IP),
            "candidates": v["candidates"], "pairing_until": v["pairing_until"]}


def devices_change(op, form):
    """승인(op="approve")·삭제("remove"). form: {ip, mac, note}. → (HTTP 상태, 결과 JSON)"""
    reg = hub_proxy.REGISTRY
    if reg is None:
        return 503, {"ok": False, "result": "noregistry", "message": "기기 등록부가 없음"}
    ip, mac = form.get("ip", "").strip(), form.get("mac", "").strip()
    if op == "approve":
        code, dev = reg.approve(ip=ip, mac=mac, note=form.get("note", ""))
    else:
        code, dev = reg.remove(ip=ip, mac=mac)
    return _CHANGE_STATUS.get(code, 500), {"ok": code in ("ok", "exists"), "result": code,
                                           "message": hub_devices.message(code, op != "approve"), "device": dev}


def _ago(t):
    d = max(0, int(time.time() - t))
    for unit, name in ((86400, "일"), (3600, "시간"), (60, "분")):
        if d >= unit:
            return f"{d // unit}{name} 전"
    return f"{d}초 전"


_PAGE_HEAD = """<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>lynnai-nas 기기 등록</title>
<style>
body{font:15px/1.5 sans-serif;max-width:60em;margin:1.5em auto;padding:0 1em}
table{border-collapse:collapse;width:100%;margin:.5em 0}
th,td{border-bottom:1px solid #bbb;padding:.35em .5em;text-align:left}
.msg{border:1px solid #888;padding:.5em .8em;margin:1em 0}
.dim{color:#666}
</style></head><body>
<h1>린나이 기기 등록</h1>
<p>등록 안 된 기기가 NAS 의 DNS 에 rkiot.net 을 물으면 아래 후보에 보인다. 내 보일러·각방 조절기가 맞으면 승인한다.
승인한 기기는 <b>DNS 를 다시 물을 때</b>부터 NAS 로 온다 — 보통 기기 전원을 껐다 켜야 한다.</p>
<p class="dim">이 페이지와 API 는 암호화 없는 HTTP 다(NAS 의 다른 API 와 같다) — 토큰도 집 LAN 을 그대로 지나간다.
MAC·IP 는 같은 LAN 에서 꾸밀 수 있으니 내 기기인지 보고 승인할 것.</p>
"""


def devices_page(view=None, msg="", edit=False, token="", ask_token=False):
    """기기 등록 페이지(HTML). view 가 None 이면 목록 없이. edit 면 승인·삭제 버튼. 넣는 값은 모두 html.escape."""
    E = html.escape
    o = [_PAGE_HEAD]
    if msg:
        o.append(f'<p class="msg">{E(msg)}</p>')
    o.append('<form method="post" action="/devices">')
    if ask_token or edit:
        o.append(f'<p><label>API 토큰 <input type="password" name="token" value="{E(token)}" size="36" required '
                 'autocomplete="off"></label> <button>목록 보기</button></p>')
    if view is not None:
        left = view["pairing_until"] - int(time.time())
        if left > 0:
            o.append(f'<p class="msg">등록 창이 열려 있음(DEVICE_PAIR) — 앞으로 {left}초 동안 보이는 후보는 바로 등록된다.</p>')
        o.append("<h2>등록된 기기</h2><table><tr><th>MAC</th><th>마지막 IP</th><th>등록</th><th>메모</th><th></th></tr>")
        for d in view["registered"]:
            key = ("mac", d["mac"]) if d["mac"] else ("ip", d["ip"])
            btn = (f'<button name="{key[0]}" value="{E(key[1])}" formaction="/devices/remove">삭제</button>' if edit else "")
            when = time.strftime("%Y-%m-%d %H:%M", time.localtime(d["added"])) if d["added"] else "-"
            o.append(f'<tr><td>{E(d["mac"]) or "(모름 — IP 로 판정)"}</td><td>{E(d["ip"]) or "-"}</td><td>{E(when)}</td>'
                     f'<td>{E(d["note"])}</td><td>{btn}</td></tr>')
        for ip in view["static"]:
            o.append(f'<tr><td class="dim">(고정 — RINNAI_IPS)</td><td>{E(ip)}</td><td>-</td>'
                     '<td class="dim">.env 에서 바꾼다</td><td></td></tr>')
        if not view["registered"] and not view["static"]:
            o.append('<tr><td colspan="5" class="dim">없음</td></tr>')
        o.append("</table><h2>후보</h2><table><tr><th>IP</th><th>MAC</th><th>경로</th><th>처음</th><th>마지막</th>"
                 "<th>횟수</th><th></th></tr>")
        for c in view["candidates"]:
            btn = f'<button name="ip" value="{E(c["ip"])}" formaction="/devices/approve">승인</button>' if edit else ""
            mac = E(c["mac"]) or "(모름)"
            if c["rinnai"]:
                mac += " — 린나이 모듈로 보임"
            o.append(f'<tr><td>{E(c["ip"])}</td><td>{mac}</td><td>{E(c["via"])}</td><td>{E(_ago(c["first"]))}</td>'
                     f'<td>{E(_ago(c["last"]))}</td><td>{int(c["count"])}</td><td>{btn}</td></tr>')
        if not view["candidates"]:
            o.append('<tr><td colspan="7" class="dim">없음 — 기기 전원을 껐다 켜면 DNS 를 물으면서 여기 보인다</td></tr>')
        o.append("</table>")
        if edit and view["candidates"]:
            o.append('<p><label>승인할 때 붙일 메모(선택) <input name="note" maxlength="64" size="30"></label></p>')
        o.append(f'<p class="dim">후보는 메모리에만 있다(최대 {hub_devices.MAX_CAND}개, NAS 를 다시 시작하면 비워진다). '
                 f'등록은 최대 {hub_devices.MAX_DEVICES}대. 삭제해도 이미 붙어 있는 연결은 끊길 때까지 남는다.</p>')
    o.append("</form></body></html>")
    return "\n".join(o)


def serve(core, hub_id, port, token, log=print):
    weak = bool(token) and len(token) < MIN_TOKEN
    if weak:
        log(f"[api] 경고: API_TOKEN 이 {MIN_TOKEN}자 미만 — /api·/metrics 를 막습니다(더 긴 토큰으로 바꿀 것)")
    tok_b = ("Bearer " + token).encode() if token else b""

    class H(BaseHTTPRequestHandler):
        def setup(self):
            self.timeout = HTTP_READ_SEC                     # 소켓 제한시간(쓰기) — 읽기는 아래 기한으로
            super().setup()
            self.rfile.close()
            self.rfile = io.BufferedReader(_DeadlineReader(self.connection, HTTP_READ_SEC))

        def log_message(self, *a):
            pass

        def _auth(self):
            if not token:
                return True
            h = self.headers.get("Authorization", "").encode("utf-8", "replace")
            if not weak and hmac.compare_digest(h, tok_b):
                return True
            self.send_response(401)
            self.send_header("WWW-Authenticate", "Bearer")
            self.end_headers()
            self.wfile.write("API 토큰 필요".encode())
            return False

        def _json(self, obj, status=200):
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(obj, ensure_ascii=False).encode())

        def _text(self, status, msg):
            self.send_response(status)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.end_headers()
            self.wfile.write(msg.encode())

        def _page(self, status, **kw):
            body = devices_page(**kw).encode()
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")           # 토큰이 든 페이지를 남기지 않게
            self.send_header("Content-Security-Policy",
                             "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(body)

        def _body(self):
            """POST 본문(JSON 객체 또는 폼) → {이름: 문자열}. 틀리면 오류로 답하고 None.
            읽기는 요청 기한(_DeadlineReader) 안에서 — 본문을 흘려 보내며 연결을 붙들지 못한다."""
            try:
                n = int(self.headers.get("Content-Length", ""))
            except ValueError:
                n = -1
            if n < 0 or n > MAX_BODY:
                self._text(411 if n < 0 else 413, "Content-Length 필요" if n < 0 else f"본문이 너무 큼(>{MAX_BODY}B)")
                return None
            raw = self.rfile.read(n)
            try:
                if len(raw) != n:
                    raise ValueError("본문이 덜 옴")
                if self.headers.get("Content-Type", "").split(";")[0].strip().lower() == "application/json":
                    v = json.loads(raw.decode("utf-8"))
                    if not isinstance(v, dict):
                        raise ValueError("JSON 객체가 아님")
                    return {k: x for k, x in v.items() if isinstance(x, str)}
                q = parse_qs(raw.decode("utf-8"), keep_blank_values=True, max_num_fields=16)
                return {k: x[0] for k, x in q.items()}
            except (ValueError, RecursionError) as e:            # JSON·UTF-8·폼 형식 오류
                self._text(400, f"본문을 못 읽음: {e}")
                return None

        def _form_token_ok(self, form):
            """폼의 token 값이 API_TOKEN 인가(Bearer 헤더와 같은 규칙: 16자 미만이면 맞아도 안 받음)."""
            got = ("Bearer " + form.get("token", "")).encode("utf-8", "replace")
            return bool(token) and not weak and hmac.compare_digest(got, tok_b)

        def _devices_get(self):
            if not token:                     # 토큰을 안 쓰는 설치: 목록은 다른 /api 처럼 열려 있고, 바꾸기는 안 받는다
                return self._page(200, view=devices_view(), msg=NO_TOKEN_MSG)
            if weak:
                return self._page(200, msg=f"API_TOKEN 이 {MIN_TOKEN}자 미만이라 API 와 이 페이지가 막혀 있음 — 더 긴 토큰으로 바꿀 것")
            self._page(200, ask_token=True)

        def _devices_post(self, p):
            form = self._body()
            if form is None:
                return
            if not token:
                return self._page(403, view=devices_view(), msg=NO_TOKEN_MSG)
            if not self._form_token_ok(form):
                return self._page(401, ask_token=True, msg="토큰이 맞지 않음" if not weak else
                                  f"API_TOKEN 이 {MIN_TOKEN}자 미만이라 막혀 있음")
            status, msg = 200, ""
            if p != "/devices":
                status, r = devices_change(p.rsplit("/", 1)[1], form)
                msg = r["message"]
                if r["ok"] and p.endswith("/approve"):
                    msg += " — 기기 전원을 껐다 켜면(DNS 를 다시 물으면) NAS 로 온다"
            self._page(status, view=devices_view(), msg=msg, edit=True, token=form.get("token", ""))

        def _send_xml(self, status, body):
            self.send_response(status)
            self.send_header("Content-Type", "application/xml; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _bootstrap(self):
            src = self.client_address[0]
            if not hub_proxy.is_device(src):   # 집 LAN 의 다른 기기가 기기 계정·암호를 받아 가지 못하게
                reg = hub_proxy.REGISTRY       # 기기처럼 굴었으면(Ck-Mac) 후보로. 등록 창이면 그 자리에서 등록돼 True
                if not (reg is not None and self.headers.get("Ck-Mac") and reg.seen(src, "http")):
                    log(f"[api] /key/server 거절: 등록 안 된 {src}")
                    self.send_response(403)
                    self.end_headers()
                    return
            key = _boot_key(self.headers.get("Ck-Service", ""), self.headers.get("Ck-Mac", ""))
            path = os.path.join(DATA, "boot", key + ".xml")
            cached = None
            try:
                with open(path, "rb") as f:
                    cached = f.read()
                if not (_boot_valid(cached) and _boot_sane(cached)):
                    cached = None
            except OSError:
                pass
            if STANDALONE:
                log("[api] /key/server(단독): 허브가 만든 답")
                return self._send_xml(200, BOOT_LOCAL)
            status = body = None
            try:
                import hub_bootstrap
                req = "GET /key/server\n" + "".join(f"{k}: {v}\n" for k, v in self.headers.items())
                status, _hdrs, body = hub_bootstrap.fetch_real_bootstrap(req)
            except Exception as e:
                log(f"[api] /key/server 클라우드 응답 없음: {e}")
            if status and 200 <= status < 300 and not (status == 200 and _boot_valid(body) and _boot_sane(body)):
                # 형식이 틀린 2xx 도 여기서 걸린다 — 검사를 못 거친 본문을 성공 응답으로 기기에 넘기지 않는다
                log(f"[api] /key/server 클라우드의 {status} 응답이 성공 형식이 아니거나 접속 주소(<ip>/<mqtt>)가 "
                    "예상과 다름 — 버림(변조 의심)")
                status = body = None
            if status == 200:
                try:
                    os.makedirs(os.path.dirname(path), mode=0o700, exist_ok=True)
                    fd = os.open(path + ".tmp", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)   # 기기 계정·암호가 들어 있음
                    with os.fdopen(fd, "wb") as f:
                        f.write(body)
                    os.replace(path + ".tmp", path)
                except OSError as e:
                    log(f"[api] 부트스트랩 저장 실패: {e}")
                return self._send_xml(200, body)
            if status:                          # 클라우드 오류: 그대로(저장 안 함)
                log(f"[api] /key/server 클라우드가 {status} / 성공 아님 — 그대로 전달")
                return self._send_xml(status, body or b"")
            if cached:
                log("[api] /key/server 클라우드 응답 없음 — 이 기기의 저장된 답")
                return self._send_xml(200, cached)
            self.send_response(502)
            self.end_headers()

        def do_GET(self):
            p = self.path.split("?", 1)[0]
            if p == "/key/server":          # 기기 부트스트랩: 인증 없이(기기가 토큰을 모른다)
                self._bootstrap()
                return
            if p == "/devices":             # 브라우저용: 헤더 토큰을 못 보내므로 토큰은 폼으로(목록은 토큰을 낸 뒤에)
                self._devices_get()
                return
            if not self._auth():
                return
            if p == "/api/v1/state":
                self._json(build_state(core, hub_id))
            elif p == "/api/v1/devices":
                self._json(devices_view())
            elif p == "/metrics":
                self.send_response(200)
                self.send_header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
                self.end_headers()
                self.wfile.write(build_metrics(core, hub_id).encode())
            elif p == "/api/stats":
                self._json({"boot": BOOT_ID, "up": int((time.time() - T0) * 1000), "heap": 0, "heap_min": 0, "count": {}})
            elif p == "/api/events":
                self._json({"boot": BOOT_ID, "up": int((time.time() - T0) * 1000), "next": 0, "lost": 0, "ev": []})
            else:
                self.send_response(404)
                self.end_headers()

        def do_POST(self):
            p = self.path.split("?", 1)[0]
            if p in ("/devices", "/devices/approve", "/devices/remove"):
                self._devices_post(p)
            elif p in ("/api/v1/devices/approve", "/api/v1/devices/remove"):
                if not token:               # 토큰 없는 설치에서는 바꾸기를 받지 않는다(읽기만 열림)
                    return self._json({"ok": False, "result": "notoken", "message": NO_TOKEN_MSG}, 403)
                if not self._auth():
                    return
                form = self._body()
                if form is not None:
                    status, r = devices_change(p.rsplit("/", 1)[1], form)
                    self._json(r, status)
            else:
                self.send_response(404)
                self.end_headers()

    class Server(ThreadingHTTPServer):
        """연결 수 상한을 둔 서버. 넘으면 받자마자 닫는다(스레드를 만들지 않음)."""
        request_queue_size = 16                      # listen 대기열(:8883 과 같게) — 거절할 연결이 몰려도 덜 밀리게
        slots = hub_proxy.Slots(HTTP_MAX_CONN, HTTP_MAX_PER_IP)
        refused = 0.0                                # 마지막으로 거절 로그를 남긴 시각(로그가 쏟아지지 않게)

        def verify_request(self, request, client_address):
            if self.slots.take(client_address[0]):
                return True
            if time.monotonic() - self.refused > 10:
                self.refused = time.monotonic()
                log(f"[api] {client_address[0]} 연결 상한 — 거절")
            return False

        def process_request(self, request, client_address):
            try:
                super().process_request(request, client_address)
            except Exception:                        # 스레드를 못 만듦: 잡은 슬롯을 돌려주고 서버가 닫게 둔다
                self.slots.give(client_address[0])
                raise

        def process_request_thread(self, request, client_address):
            try:
                super().process_request_thread(request, client_address)
            finally:
                self.slots.give(client_address[0])

        def handle_error(self, request, client_address):
            e = sys.exc_info()[1]                    # 상대가 끊은 것 등 — 한 줄만(기본은 스택 전체를 찍는다)
            log(f"[api] {client_address[0]} 요청 처리 오류: {type(e).__name__}:{e}")

    httpd = Server(("0.0.0.0", port), H)
    log(f"[api] 서버 인터페이스 :{port} (토큰 {'약함 — 막음' if weak else '있음' if token else '없음 — LAN 에 열림'}"
        f", 동시 연결 {HTTP_MAX_CONN}·출발지당 {HTTP_MAX_PER_IP}·요청 기한 {HTTP_READ_SEC}초)")
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd
