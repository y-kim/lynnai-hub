#!/usr/bin/env python3
"""자동 단독 동작(폴백) 시험 — 가짜 기기와 가짜 린나이 서버를 이 PC 안에서 띄워 hub_proxy 를 돌려 본다.

    python3 test_fallback.py        (openssl 명령이 있어야 한다 — 시험용 인증서를 만든다)

확인하는 것: 서버가 로그인을 받아 주면 그대로 중계 / 서버에 연결이 안 되면 두 번째 실패부터 허브가 직접 받음 /
서버가 돌아오면 프록시로 복귀 / 로그인만 거절하는 서버도 실패로 셈 / 다른 기기가 붙어 있으면 한 기기의 거절은 안 셈.
"""
import os, socket, ssl, struct, subprocess, sys, tempfile, threading, time

TMP = tempfile.mkdtemp(prefix="lynnai-nas-test-")


def cert(name, cn):
    crt, key = os.path.join(TMP, name + ".crt"), os.path.join(TMP, name + ".key")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=" + cn,
                    "-keyout", key, "-out", crt], check=True, capture_output=True)
    return crt, key


def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p


CLOUD_CRT, CLOUD_KEY = cert("cloud", "dev2.ckpush.com")
HUB_CRT, HUB_KEY = cert("hub", "lynnai-nas-test")
CLOUD_PORT, HUB_PORT = free_port(), free_port()
os.environ.update(MODE="proxy", UPSTREAM_IP="127.0.0.1", UPSTREAM_PORT=str(CLOUD_PORT), UPSTREAM_CA=CLOUD_CRT,
                  NAS_CERT=HUB_CRT, NAS_KEY=HUB_KEY, MQTT_PORT=str(HUB_PORT), RINNAI_IPS="127.0.0.1",
                  FB_PROBE_S="1", ACCEPT_WAIT_S="2", AUTO_FALLBACK="1")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hub_proxy                                             # noqa: E402  (환경변수를 먼저 정해야 한다)

hub_proxy.log = lambda m, f=None: None                       # 시험 중엔 조용히


class Cloud:
    """가짜 린나이 서버. behave: accept(로그인 수락) / refuse(코드 5 로 거절) / refuse_bad(아이디가 bad 인 기기만 거절)"""
    def __init__(self):
        self.behave = "accept"
        self.ls = None
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(CLOUD_CRT, CLOUD_KEY)

    def up(self):
        self.ls = socket.socket(); self.ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.ls.bind(("127.0.0.1", CLOUD_PORT)); self.ls.listen(8)
        threading.Thread(target=self._loop, args=(self.ls,), daemon=True).start()

    def down(self):
        ls, self.ls = self.ls, None
        try: ls.shutdown(socket.SHUT_RDWR)
        except OSError: pass
        ls.close()

    def _loop(self, ls):
        while True:
            try: c, _ = ls.accept()
            except OSError: return
            threading.Thread(target=self._one, args=(c,), daemon=True).start()

    def _one(self, c):
        try:
            s = self.ctx.wrap_socket(c, server_side=True)
            d = s.recv(4096)
            if not d or d[0] >> 4 != 1:
                s.close(); return                                # 확인용 연결(TLS 만 맺고 끊음)
            bad = d.endswith(b"bad")
            rc = 5 if self.behave == "refuse" or (self.behave == "refuse_bad" and bad) else 0
            s.sendall(bytes([0x20, 2, 0, rc]))
            if rc:
                s.close(); return
            while s.recv(4096):
                pass
        except Exception:
            pass
        finally:
            try: c.close()
            except Exception: pass


def connect_packet(cid):
    body = b"\x00\x04MQTT\x04\x02\x00\x3c" + struct.pack(">H", len(cid)) + cid
    return bytes([0x10, len(body)]) + body


class Device:
    """가짜 기기: 허브에 TLS 로 붙어 로그인(CONNECT)을 보내고 답(CONNACK 코드)을 받는다."""
    def __init__(self, cid=b"dev"):
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
        self.s = ctx.wrap_socket(socket.create_connection(("127.0.0.1", HUB_PORT), timeout=5))
        self.s.settimeout(6)
        self.s.sendall(connect_packet(cid))

    def connack(self):
        """서버(또는 허브)의 답 코드. 답 없이 끊기면 None."""
        try:
            d = self.s.recv(4)
        except (OSError, ssl.SSLError):
            return None
        return d[3] if len(d) == 4 and d[0] == 0x20 else None

    def closed_by_hub(self, within=8):
        self.s.settimeout(within)
        try:
            return self.s.recv(16) == b""
        except (OSError, ssl.SSLError):
            return False

    def close(self):
        try: self.s.close()
        except Exception: pass


passed = failed = 0


def check(ok, what):
    global passed, failed
    if ok: passed += 1
    else: failed += 1
    print(("  OK   " if ok else "  FAIL ") + what)


def wait_for(pred, sec):
    end = time.time() + sec
    while time.time() < end:
        if pred(): return True
        time.sleep(0.05)
    return pred()


FB = hub_proxy.FB
cloud = Cloud(); cloud.up()
threading.Thread(target=hub_proxy.run, daemon=True).start()
time.sleep(0.3)

print("[1] 서버가 로그인을 받아 주면 그대로 중계")
d = Device()
check(d.connack() == 0 and not FB.active, "기기가 서버의 수락을 받음, 폴백 아님")
check(wait_for(lambda: FB.ok_links == 1, 2), "로그인된 연결 1개로 셈")
d.close()
check(wait_for(lambda: FB.ok_links == 0, 3), "끊으면 0개")

print("[2] 서버에 연결이 안 되면: 첫 번째는 실패, 두 번째부터 허브가 직접 받음")
cloud.down()
d = Device(); check(d.connack() is None and not FB.active, "첫 실패: 답 없이 끊김, 아직 폴백 아님"); d.close()
d = Device()
check(d.connack() == 0 and FB.active, "두 번째 실패: 허브가 로그인을 받아 줌(폴백)")
p = hub_proxy.mode_payload()
check(p[0] == 0 and p[1] == 1 and p[7] == hub_proxy.WHY_CONNECT and len(p) == 9, "모드 알림: 설정은 프록시, 폴백 중, 까닭 = 연결 안 됨")
check(hub_proxy.mode_json()["fallback_why"] == "connect", "상태 API 에도 같은 까닭")

print("[3] 서버가 돌아오면 프록시로 복귀하고 직접 받던 연결을 끊음")
cloud.up()
check(wait_for(lambda: not FB.active, 6), "확인(보안 연결) 두 번 성공 → 폴백 끝")
check(d.closed_by_hub(), "허브가 직접 받던 연결을 닫음(기기가 다시 붙게)")
d.close()
d = Device(); check(d.connack() == 0 and not FB.active, "다시 붙으면 서버로 중계"); d.close()
wait_for(lambda: FB.ok_links == 0, 3)

print("[4] 연결은 되는데 로그인을 거절하는 서버도 실패로 셈")
cloud.behave = "refuse"
d = Device(); check(d.connack() == 5, "첫 거절: 기기는 서버의 거절을 그대로 받음"); d.close()
check(wait_for(lambda: FB.fails == 1, 2) and not FB.active, "실패 1번")
d = Device(); d.connack(); d.close()
check(wait_for(lambda: FB.active, 2) and FB.why == hub_proxy.WHY_REFUSED and FB.rc == 5, "두 번째 거절 → 폴백, 까닭 = 로그인 거절(코드 5)")
d = Device(); check(d.connack() == 0, "그 뒤로는 허브가 직접 받음"); d.close()

print("[5] 자동 폴백을 끄면 바로 나옴")
hub_proxy.request("fallback=0")
check(not FB.active and not FB.on, "꺼짐, 폴백 끝")
hub_proxy.request("fallback=1")
cloud.behave = "accept"
time.sleep(0.2)

print("[6] 다른 기기가 서버에 붙어 있으면 한 기기의 거절은 세지 않음")
cloud.behave = "refuse_bad"
good = Device(b"good"); check(good.connack() == 0 and wait_for(lambda: FB.ok_links == 1, 2), "멀쩡한 기기 연결됨")
for _ in range(3):
    b = Device(b"bad"); b.connack(); b.close(); time.sleep(0.3)
check(not FB.active and FB.fails == 0, "거절당한 기기가 세 번 실패해도 폴백 아님")
good.close()

print(f"\n== {passed}통과 / {failed}실패 ==")
sys.exit(1 if failed else 0)
