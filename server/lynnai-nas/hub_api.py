"""
hub_api.py (대응 ESP32: hub_api.c) -- 서버 인터페이스(장기 기록·그래프). ESP32 펌웨어 hub_api.c 와 같은 형식이라
lynnai-hub-watch·Prometheus 가 NAS 든 ESP32 든 똑같이 읽는다.

  GET /api/v1/state   지금 상태(JSON): 보일러(해독+raw), 각방, 연결, time
  GET /metrics        Prometheus 텍스트(lynnai_*)
  GET /api/stats      부팅 id·up(watch 호환)   GET /api/events  (NAS 는 이벤트 링이 없어 빈 목록)
토큰(API_TOKEN) 있으면 Authorization: Bearer 필요. 16자 미만이면 약해서 /api·/metrics 를 막는다(경고 로그).

GET /key/server(기기 부트스트랩, 인증 없음) — ESP32 hub_proxy_bootstrap_handler 와 같은 규칙:
  proxy      : 클라우드에 다시 묻고, 정상(200 + 성공 XML)이면 (서비스, MAC)별로 DATA_DIR/boot/ 에 저장해 그대로.
               클라우드 오류는 그 상태·본문 그대로(저장 안 함), 응답이 없으면 이 기기의 저장분, 없으면 502.
               성공 응답이라도 <ip> 가 rkiot.net 이름이 아니거나 MQTT 포트가 다르면(변조) 버린다 — 응답 없음과 같게.
  standalone : 허브가 만든 답만(uid/pass 는 자리 표시 — 단독 브로커는 무엇이든 받는다). 저장한 클라우드 계정은 안 내준다.
  RINNAI_IPS 가 있으면 그 기기들의 요청만 받는다(나머지 403).
"""
import hmac
import json
import os
import re
import random
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KIND_F06, KIND_F09 = 6, 9
DATA = os.environ.get("DATA_DIR", "/data")
STANDALONE = os.environ.get("MODE", "proxy") == "standalone"
# /key/server 는 등록된 린나이 기기에게만(집 LAN 의 다른 기기가 기기 계정·암호를 받아 가지 못하게). 비우면 제한 없음
DEVICE_IP = {ip.strip() for ip in os.environ.get("RINNAI_IPS", "").replace(";", ",").split(",") if ip.strip()}
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


def serve(core, hub_id, port, token, log=print):
    weak = bool(token) and len(token) < MIN_TOKEN
    if weak:
        log(f"[api] 경고: API_TOKEN 이 {MIN_TOKEN}자 미만 — /api·/metrics 를 막습니다(더 긴 토큰으로 바꿀 것)")
    tok_b = ("Bearer " + token).encode() if token else b""

    class H(BaseHTTPRequestHandler):
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

        def _json(self, obj):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(obj, ensure_ascii=False).encode())

        def _send_xml(self, status, body):
            self.send_response(status)
            self.send_header("Content-Type", "application/xml; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _bootstrap(self):
            if DEVICE_IP and self.client_address[0] not in DEVICE_IP:
                log(f"[api] /key/server 거절: 등록 안 된 {self.client_address[0]}")
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
            if status == 200 and _boot_valid(body) and not _boot_sane(body):
                log("[api] /key/server 클라우드 응답의 접속 주소(<ip>/<mqtt>)가 예상과 다름 — 버림(변조 의심)")
                status = body = None
            if status == 200 and _boot_valid(body):
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
            if not self._auth():
                return
            if p == "/api/v1/state":
                self._json(build_state(core, hub_id))
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

    httpd = ThreadingHTTPServer(("0.0.0.0", port), H)
    log(f"[api] 서버 인터페이스 :{port} (토큰 {'약함 — 막음' if weak else '있음' if token else '없음 — LAN 에 열림'})")
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
