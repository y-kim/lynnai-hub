"""
hub_api.py (대응 ESP32: hub_api.c) -- 서버 인터페이스(장기 기록·그래프). ESP32 펌웨어 hub_api.c 와 같은 형식이라
lynnai-hub-watch·Prometheus 가 NAS 든 ESP32 든 똑같이 읽는다.

  GET /api/v1/state   지금 상태(JSON): 보일러(해독+raw), 각방, 연결, time
  GET /metrics        Prometheus 텍스트(lynnai_*)
  GET /api/stats      부팅 id·up(watch 호환)   GET /api/events  (NAS 는 이벤트 링이 없어 빈 목록)
토큰(API_TOKEN) 있으면 Authorization: Bearer 필요.
"""
import json
import os
import random
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KIND_F06, KIND_F09 = 6, 9
ZONES = [("z1", "안방", 0, 3, 4), ("z2", "방2", 7, 10, 11), ("z3", "거실", 42, 45, 46)]
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
         "current": r[z[3]], "target": r[z[4]]} for z in ZONES]}


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
                v = (int(bool(r[z[idx]] & mask)) if mask else r[z[idx]])
                L.append(f'lynnai_zone_{metric}{{mac="{mac}",zone="{z[0]}",name="{z[1]}"}} {v}')
    return "\n".join(L) + "\n"


def serve(core, hub_id, port, token, log=print):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _auth(self):
            if not token:
                return True
            h = self.headers.get("Authorization", "")
            if h == "Bearer " + token:
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

        def do_GET(self):
            p = self.path.split("?", 1)[0]
            if p == "/key/server":          # 기기 부트스트랩: 인증 없이(기기가 토큰을 모른다)
                try:
                    import hub_bootstrap
                    req = "GET /key/server\n" + "".join(f"{k}: {v}\n" for k, v in self.headers.items())
                    status, _hdrs, body = hub_bootstrap.fetch_real_bootstrap(req)
                    self.send_response(status)
                    self.send_header("Content-Type", "application/xml; charset=utf-8")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                except Exception as e:
                    log(f"[api] /key/server 중계 실패: {e}")
                    self.send_response(502)
                    self.end_headers()
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
    log(f"[api] 서버 인터페이스 :{port} (토큰 {'있음' if token else '없음'})")
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
