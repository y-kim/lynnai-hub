#!/usr/bin/env python3
"""hub_proxy.py (대응 ESP32: hub_proxy.c) -- lynnai-nas 의 기기 연결(:8883). 내 보일러 TLS 를 받아(우리 인증서)
  proxy(기본)   : 진짜 린나이 클라우드로 다시 TLS 로 이어 준다. 양쪽을 그대로 포워딩하므로 린나이 앱은 계속 동작.
  standalone    : 클라우드에 안 붙고 허브가 MQTT 상대(CONNACK·SUBACK·PUBACK·PINGRESP)가 된다. 상태는 코어로,
                  상태 요청·시간 응답·SmartThings 명령은 코어가 넣는다(MODE=standalone).
ESP32 펌웨어 hub_proxy.c 의 파이썬 판.
(원본: Pi 의 rkprobe/bridge.py — 하드코딩 경로·기기 IP 를 환경변수로 바꾸고, 평문 payload 로깅은 기본 끔.)

환경변수: NAS_CERT, UPSTREAM_IP/UPSTREAM_PORT, UPSTREAM_SNI,
          RINNAI_IPS(고정 등록할 기기 IP, 쉼표 — 선택. 비우면 고정 등록 없음: 승인한 기기(REGISTRY = hub_devices)만 받는다.
                     IPv4 주소가 아닌 항목이 있으면 시작하지 않는다. 모든 출발지를 받으려면 any 를 명시),
          UPSTREAM_CA(클라우드 사설 CA, 기본 이 폴더의 cloud_ca.crt — 비우거나 파일이 없으면 클라우드에 붙지 않음),
          MQTT_PORT, MAX_CONN, DEBUG_MQTT(1이면 복호화 payload 까지 로그 — 비밀값 포함, 평소엔 끔),
          AUTO_FALLBACK(기본 1: 프록시인데 린나이 서버가 안 되면 그동안 허브가 직접 받는다. 0 이면 끔).

자동 단독 동작(폴백) — ESP32 hub_proxy.c 와 같은 규칙:
  들어가기: 클라우드 쪽이 FB_FAILS 번 연달아 실패. "성공"은 서버가 기기의 로그인을 받아 준 것(CONNACK 코드 0)이다 —
            연결·보안 연결이 안 될 때뿐 아니라 로그인 거절, ACCEPT_WAIT_S 초 무응답, 수락 전에 끊는 것도 실패로 센다.
            다만 다른 기기가 로그인된 채 붙어 있으면 한 기기의 로그인 실패는 세지 않는다(서버는 멀쩡하다).
  나오기:   FB_PROBE_S 초마다 서버에 보안 연결까지 해 보고 두 번 연달아 되면 직접 받던 연결을 끊는다(기기가 프록시로
            다시 붙는다). 돌아갔다가 또 실패하면 간격을 두 배씩(최대 FB_PROBE_MAX_S)."""
import ipaddress, os, selectors, socket, ssl, struct, threading, time
import upstream

CERT = os.environ.get("NAS_CERT", "/data/certs/server.crt")
KEY = os.environ.get("NAS_KEY", "/data/certs/server.key")
LISTEN = ("0.0.0.0", int(os.environ.get("MQTT_PORT", "8883")))
UP_PORT = int(os.environ.get("UPSTREAM_PORT", "8883"))
STANDALONE = os.environ.get("MODE", "proxy") == "standalone"
IDLE_SEC = 200
UP_SNI = os.environ.get("UPSTREAM_SNI", "www.rkiot.net")
# 린나이 클라우드의 사설 CA "ckstack"(2123년까지). 서버 인증서 CN 이 dev2.ckpush.com(접속 이름과 다름)이라 체인 + 그 CN 을 고정.
UP_CA = os.environ.get("UPSTREAM_CA", os.path.join(os.path.dirname(os.path.abspath(__file__)), "cloud_ca.crt"))
UP_CN = os.environ.get("UPSTREAM_CN") or "dev2.ckpush.com"   # 클라우드 인증서의 실제 이름(SAN 없이 CN 만) — 체인에 더해 이름도 고정
DEBUG_MQTT = os.environ.get("DEBUG_MQTT", "") not in ("", "0")   # payload 에 비밀값이 있어 기본 끔
TYPES = {1:"CONNECT",2:"CONNACK",3:"PUBLISH",4:"PUBACK",8:"SUBSCRIBE",9:"SUBACK",
         10:"UNSUBSCRIBE",12:"PINGREQ",13:"PINGRESP",14:"DISCONNECT"}

def log(m, f=None):
    import datetime
    print(f"{datetime.datetime.now().isoformat(timespec='seconds')} [relay] {m}", flush=True)

MAX_PKT = 64 * 1024        # MQTT 패킷 하나의 상한(실제 기기·클라우드는 수백 바이트). 넘으면 연결을 끊는다
MAX_CONN = int(os.environ.get("MAX_CONN") or "16")   # :8883 동시 연결 상한(등록된 기기는 따로 센다 — Slots)
MAX_PER_IP = 4             # 한 출발지당 — LAN 기기 하나가 슬롯을 다 잡아 진짜 기기를 막지 못하게
MAX_PENDING = 32           # 기기에 끼워 넣을 명령 대기 상한
RELAY_READ_SEC = 1         # 중계 중 한 번 읽기의 최대 대기(넘으면 다음 바퀴로 — TLS 1.3 세션 티켓처럼 앱 데이터가 없는 기록 때문)
# 소켓 기다리기: select.select() 는 파일 번호가 1024 이상이면 ValueError 라 poll 기반으로(없는 플랫폼은 그 기본값)
Selector = getattr(selectors, "PollSelector", selectors.DefaultSelector)

class FrameError(ValueError):
    pass

def frame(buf, i):
    """buf[i:] 의 MQTT 패킷 하나: (b0, 본문 시작, 끝) / 덜 왔으면 None / 길이 필드가 깨졌거나 MAX_PKT 초과면 FrameError."""
    if len(buf) - i < 2: return None
    mult = 1; val = 0; j = i + 1
    while True:
        if j >= len(buf): return None
        if j - i > 4: raise FrameError("길이 필드 깨짐")
        enc = buf[j]; val += (enc & 0x7f) * mult; j += 1
        if not enc & 0x80: break
        mult *= 128
    if val > MAX_PKT: raise FrameError(f"패킷이 너무 큼({val}B)")
    if len(buf) - j < val: return None
    return buf[i], j, j + val

def _show(b0, body, arrow, on_publish):
    ptype = b0 >> 4
    name = TYPES.get(ptype, f"?{ptype}")
    if ptype == 3:  # PUBLISH
        tl = (body[0] << 8) | body[1]; topic = body[2:2+tl].decode("utf-8","replace")[:200]
        rest = body[2+tl:]
        if (b0 >> 1) & 3 > 0: rest = rest[2:]   # strip packet id if QoS>0
        if DEBUG_MQTT:
            log(f"{arrow} PUBLISH {topic}  {rest.decode('utf-8','replace')}")   # 비밀값 포함 — DEBUG_MQTT 일 때만
        else:
            log(f"{arrow} PUBLISH {topic}  ({len(rest)}B)")
        if on_publish:
            try: on_publish(topic, rest)
            except Exception as e: log(f"{arrow} on_publish 오류: {e}")
    elif ptype == 8:  # SUBSCRIBE
        rest = body[2:]; topics = []
        while len(rest) >= 2:
            tl = (rest[0] << 8) | rest[1]; topics.append(rest[2:2+tl].decode("utf-8","replace")[:200]); rest = rest[2+tl+1:]
        log(f"{arrow} SUBSCRIBE {topics}")
    elif ptype == 1:  # CONNECT -- variable header: proto name(2+len) + level(1) + flags(1) + keepalive(2), then payload: clientId(2+len)
        pl = (body[0] << 8) | body[1]
        off = 2 + pl + 1 + 1 + 2   # skip proto name, level, flags, keepalive
        cl = (body[off] << 8) | body[off+1]
        log(f"{arrow} CONNECT clientId={body[off+2:off+2+cl].decode('utf-8', 'replace')[:100]!r}")
    elif ptype == 14:
        log(f"{arrow} {name}")

def parse(buf, arrow, on_publish=None):
    """buf 에서 다 온 MQTT 패킷을 꺼내 로그/스눕하고 남은 것(덜 온 패킷)을 돌려준다.
    on_publish(topic, payload_bytes) 가 있으면 PUBLISH 마다 부른다(상태 스눕용). 틀(길이)이 깨지면 FrameError,
    패킷 안쪽이 이상한 것은 그 패킷만 건너뛴다."""
    i = 0
    while True:
        f = frame(buf, i)
        if f is None: break
        b0, j, i = f
        try: _show(b0, buf[j:i], arrow, on_publish)
        except Exception as e: log(f"{arrow} 패킷 해석 실패({TYPES.get(b0 >> 4, b0 >> 4)}): {e}")
    return buf[i:]

def pump(src, dst, arrow, decode=True, on_publish=None, wlock=None, act=None):
    """src → dst 로 포워딩. decode 면 MQTT 로 로그/스눕.
    wlock 이 있으면 다 온 패킷만 그 락을 쥐고 보낸다 → dst 쪽 흐름이 늘 패킷 경계라 다른 스레드가
    같은 락으로 명령을 끼워 넣어도 안전(cloud→device 방향). act=[마지막 활동 시각]: 양쪽이 같이 쓰고,
    둘 다 IDLE_SEC 동안 조용하면 끝낸다(소켓 제한시간은 IDLE_SEC)."""
    buf = b""; why = "?"
    try:
        while True:
            try:
                d = src.recv(4096)
            except socket.timeout:
                if act is not None and time.monotonic() - act[0] < IDLE_SEC: continue
                why = "유휴"; break
            if not d: why = "EOF(peer FIN)"; break
            if act is not None: act[0] = time.monotonic()
            if not decode:
                dst.sendall(d); continue
            if wlock is None:
                dst.sendall(d)
                buf = parse(buf + d, arrow, on_publish)
            else:
                buf += d
                left = parse(buf, arrow, on_publish)
                whole = buf[:len(buf) - len(left)]; buf = left
                if whole:
                    with wlock: dst.sendall(whole)
    except Exception as e:
        why = f"{type(e).__name__}:{e}"
    finally:
        log(f"{arrow} pump end: {why}")
        # SSLSocket.shutdown 은 TLS 객체를 떼어 내 다른 스레드의 송수신이 평문으로 새므로 소켓 쪽 shutdown 을 직접 부른다
        try: socket.socket.shutdown(dst, socket.SHUT_WR)
        except Exception: pass

def parse_devices(raw):
    """RINNAI_IPS 값 → (고정 등록 IP 집합, 모두 받기). 'any' 만 모두 받기다. 비어 있으면 (빈 집합, False) —
    고정 등록 없음이지 모두 받기가 아니다(승인한 기기만 받는다). IPv4 주소가 아닌 항목이 있으면 ValueError(시작을 거부하게)."""
    raw = (raw or "").strip()
    if raw.lower() == "any":
        return set(), True
    ips = set()
    for tok in raw.replace(";", ",").split(","):
        tok = tok.strip()
        if not tok: continue
        try: ips.add(str(ipaddress.IPv4Address(tok)))
        except ValueError: raise ValueError(f"RINNAI_IPS 의 {tok!r} 는 IPv4 주소가 아님(쉼표로 나눈 IP, 또는 any)") from None
    return ips, False

# 중계(TLS 종단)할 기기 = 등록된 린나이 기기: RINNAI_IPS 에 적은 고정 IP + 승인한 기기(REGISTRY).
# 그 밖의 클라이언트는 passthrough(단독이면 끊음). hub_api(/key/server)·nas_hub(DNS, 시작 검사)도 이 값을 쓴다.
# 값이 잘못됐으면 DEVICE_ERR 에 이유 — 고정 등록으로는 아무도 기기로 안 본다(nas_hub·run 이 시작을 거부).
try:
    DEVICE_IP, DEVICE_ANY = parse_devices(os.environ.get("RINNAI_IPS", "")); DEVICE_ERR = ""
except ValueError as e:
    DEVICE_IP, DEVICE_ANY, DEVICE_ERR = set(), False, str(e)

REGISTRY = None   # 승인한 기기 등록부(hub_devices.Devices). nas_hub 가 넣는다. 없으면 RINNAI_IPS 만 본다.

def is_device(ip):
    if DEVICE_ANY or ip in DEVICE_IP: return True
    return REGISTRY is not None and REGISTRY.is_registered(ip)

def _apart(ip):
    """연결 수를 따로 세는 출발지(등록된 기기)인가. any 로는 기기를 가려낼 수 없어 넣지 않는다."""
    return ip in DEVICE_IP or (REGISTRY is not None and REGISTRY.is_registered(ip, learn=False))

def passthrough(raw, addr):
    """Non-device client (e.g. the NAS logger): forward its TLS straight to the real cloud so its own
    cert check succeeds. We never terminate TLS here, so we do not break pinned clients."""
    try:
        up = upstream.connect(UP_PORT, timeout=10, log=log)
    except Exception as e:
        log(f"{addr[0]} passthrough upstream fail: {e}"); 
        try: raw.close()
        except Exception: pass
        return
    act = [time.monotonic()]
    raw.settimeout(IDLE_SEC); up.settimeout(IDLE_SEC)
    t1 = threading.Thread(target=pump, args=(raw, up, "P>"), kwargs={"decode": False, "act": act}, daemon=True)
    t1.start()
    pump(up, raw, "P<", decode=False, act=act)                 # 한쪽은 이 스레드에서(스레드 수 줄이기)
    t1.join()
    for sck in (raw, up):
        try: sck.close()
        except Exception: pass

CORE = None   # 설정되면 상태 스눕 + 명령 주입을 한다(hub_core.HubCore). 없으면 순수 중계만.

# ---------------------------------------------------------------- 자동 단독 동작(폴백)
FB_FAILS = 2
FB_PROBE_S = int(os.environ.get("FB_PROBE_S") or "60")
FB_PROBE_MAX_S = 600
ACCEPT_WAIT_S = int(os.environ.get("ACCEPT_WAIT_S") or "15")
# 까닭(상태 글에 그대로 보인다) — 펌웨어 FB_WHY_* 와 같은 번호
WHY_CONNECT, WHY_TLS, WHY_REFUSED, WHY_NOREPLY, WHY_CLOSED, WHY_DNS = 2, 3, 4, 5, 6, 7
WHY_NAME = {0: "", 2: "connect", 3: "tls", 4: "refused", 5: "noreply", 6: "closed", 7: "dns"}
WHY_TEXT = {0: "린나이 서버 접속 불가", 2: "린나이 서버 연결 안 됨", 3: "린나이 서버 보안 연결 실패", 4: "린나이 서버 로그인 거절",
            5: "린나이 서버 로그인 응답 없음", 6: "린나이 서버가 연결을 끊음", 7: "린나이 서버 주소 조회 실패"}
ON_MODE = None                # 모드·폴백이 바뀌면 부른다(nas_hub 가 스마트홈으로 알리는 함수를 넣는다)


class Fallback:
    def __init__(self):
        self.lock = threading.Lock()
        self.on = os.environ.get("AUTO_FALLBACK", "1") not in ("0", "")
        self.active = False
        self.fails = 0
        self.why = self.rc = 0
        self.since = 0            # 폴백이 시작된 시각(유닉스 초)
        self.probe_s = FB_PROBE_S
        self.epoch = 0            # 폴백에 들어갈 때마다 +1 — 옛 확인 스레드가 알아채고 끝남
        self.gen = 0              # 바뀌면 지금의 연결들을 닫는다(기기가 새 방식으로 다시 붙게)
        self.ok_links = 0         # 서버가 로그인을 받아 준 채 이어져 있는 연결 수

    def local(self):
        """허브가 기기를 직접 받아야 하나(단독 모드이거나 폴백 중)."""
        return STANDALONE or self.active

    def accepted(self, delta):
        with self.lock:
            self.ok_links += delta
            if delta > 0:
                self.fails = 0
                self.probe_s = FB_PROBE_S

    def fail(self, why, rc=0, after_login=False):
        """클라우드 쪽이 실패했다. 이 연결을 허브가 직접 받아야 하면(폴백 시작 또는 이미 폴백 중) True."""
        if STANDALONE or not self.on:
            return False
        entered = False
        with self.lock:
            if self.active:
                return True
            if after_login and self.ok_links > 0:
                log("이 기기만 서버 로그인이 안 됨(다른 기기는 연결 중) — 폴백으로 세지 않음")
                return False
            self.fails += 1
            if self.fails >= FB_FAILS:
                self.active, self.why, self.rc, self.since = True, why, rc, int(time.time())
                self.epoch += 1
                self.gen += 1
                entered = True
                epoch = self.epoch
        if entered:
            log(f"폴백 시작: {FB_FAILS}번 연달아 실패({WHY_TEXT.get(why, '?')}) — 허브가 기기를 직접 받는다"
                f"({self.probe_s}초마다 다시 확인)")
            threading.Thread(target=self._probe, args=(epoch,), daemon=True).start()
            _notify()
        return entered

    def set_on(self, on):
        """자동 폴백 설정(실행 중에만 — 다시 시작하면 AUTO_FALLBACK 값으로)."""
        with self.lock:
            changed = self.on != on
            self.on = on
            left = changed and not on and self.active
            if left:
                self.active = False
                self.fails = 0
                self.gen += 1
        if changed:
            log(f"자동 폴백 {'켬' if on else '끔'}")
        _notify()

    def _probe(self, epoch):
        ok = 0
        while True:
            wait = min(10, self.probe_s) if ok else self.probe_s      # 한 번 됐으면 곧 한 번 더(두 번 연달아 돼야 돌아간다)
            end = time.monotonic() + wait
            while time.monotonic() < end and self.active and self.epoch == epoch:
                time.sleep(0.2 if wait < 5 else 1)
            if not self.active or self.epoch != epoch:
                return
            try:
                up = cloud_tls(timeout=5)
                up.close()
                ok += 1
            except Exception as e:
                ok = 0
                why = _why_of(e)
                if why != self.why:                # 까닭이 달라졌으면(연결·보안 연결 단계만 여기서 확인된다) 상태 글도 바꾼다
                    self.why, self.rc = why, 0
                    _notify()
                continue
            if ok < 2:
                continue
            with self.lock:
                left = self.active and self.epoch == epoch
                if left:
                    self.active = False
                    self.fails = 0
                    self.probe_s = min(self.probe_s * 2, FB_PROBE_MAX_S)      # 또 실패하면 더 천천히
                    self.gen += 1                  # 허브가 직접 받던 연결을 끊는다 → 기기가 프록시로 다시 붙음
            if left:
                log(f"폴백 끝: 린나이 서버가 다시 열림({(int(time.time()) - self.since) // 60}분 만에) — 프록시로 돌아감")
                _notify()
            return


FB = Fallback()


def _notify():
    if ON_MODE is not None:
        try: ON_MODE()
        except Exception as e: log(f"모드 알림 실패: {e}")


def mode_payload():
    """HUB_RK_CMD_MODE(0x0b): [정해 둔 모드 0 프록시|1 단독][폴백 중][자동 폴백 설정][폴백 시작 시각 u32][까닭][거절 코드]"""
    fb = FB.active and not STANDALONE
    return (bytes([1 if STANDALONE else 0, 1 if fb else 0, 1 if FB.on else 0]) + struct.pack("<I", FB.since if fb else 0)
            + bytes([FB.why if fb else 0, FB.rc if fb else 0]))


def mode_json():
    fb = FB.active and not STANDALONE
    return {"set": "standalone" if STANDALONE else "proxy", "fallback": fb, "fallback_on": FB.on,
            "fallback_since": FB.since if fb else 0, "fallback_why": WHY_NAME.get(FB.why, "") if fb else ""}


def request(text):
    """스마트홈의 "hub …" 요청. 자동 폴백은 실행 중에 바꿀 수 있고, 모드(프록시/단독)는 MODE 환경변수로만 정한다."""
    k, _, v = text.strip().partition("=")
    if k == "fallback" and v in ("0", "1"):
        FB.set_on(v == "1")
    elif k == "mode":
        log(f"모드 바꾸기 요청({v}) — NAS 판은 MODE 환경변수로만 정한다(.env 를 고쳐 다시 시작). 지금 값을 다시 알림")
        _notify()
    else:
        log(f"모르는 hub 요청: {text[:40]}")


class CloudError(Exception):
    def __init__(self, why, msg):
        super().__init__(msg)
        self.why = why


def _why_of(e):
    return e.why if isinstance(e, CloudError) else WHY_CONNECT


def cloud_tls(timeout=10):
    """진짜 클라우드에 보안 연결(TLS)까지 맺어 돌려준다. 안 되면 CloudError(까닭)."""
    if not UP_CA or not os.path.exists(UP_CA):
        raise CloudError(WHY_TLS, "클라우드 CA 없음 — 검증 없이는 붙지 않음")
    cctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    cctx.check_hostname = False                    # 접속 이름(www.rkiot.net)과 인증서 이름이 달라 아래에서 직접 비교
    cctx.verify_mode = ssl.CERT_REQUIRED; cctx.load_verify_locations(UP_CA)   # 가짜 클라우드 차단
    cctx.minimum_version = ssl.TLSVersion.TLSv1_2   # 클라우드는 TLS 1.3 — 옛 버전으로 내려가지 않게(기기 쪽만 1.0 허용)
    try:
        raw = upstream.connect(UP_PORT, timeout=timeout, log=log)
    except upstream.NoAddress as e:
        raise CloudError(WHY_DNS, str(e))
    except OSError as e:
        raise CloudError(WHY_CONNECT, f"연결 안 됨: {e}")
    try:
        up = cctx.wrap_socket(raw, server_hostname=UP_SNI)
    except Exception as e:
        raw.close()
        raise CloudError(WHY_TLS, f"TLS 실패: {e}")
    cn = dict(x[0] for x in (up.getpeercert() or {}).get("subject", ())).get("commonName")
    if cn != UP_CN:
        up.close()
        raise CloudError(WHY_TLS, f"클라우드 인증서 이름 다름({cn!r} ≠ {UP_CN!r})")
    return up

def handle(raw, addr):
    # 등록된 린나이 기기만 TLS 종단(중계), 나머지(로거·헬스체크 등)는 passthrough 로 안 깨뜨린다.
    if not is_device(addr[0]):
        if STANDALONE:                       # 단독 모드는 밖으로 아무것도 잇지 않는다(README 의 약속)
            log(f"{addr[0]} 등록 안 된 출발지 — 단독 모드라 끊음")
            try: raw.close()
            except Exception: pass
            return
        passthrough(raw, addr); return
    sctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    try:
        sctx.minimum_version = ssl.TLSVersion.TLSv1; sctx.set_ciphers("DEFAULT@SECLEVEL=0")
    except Exception: pass
    sctx.load_cert_chain(CERT, KEY)
    raw.settimeout(30)
    try: down = sctx.wrap_socket(raw, server_side=True)
    except Exception as e:
        log(f"{addr[0]} down TLS fail: {e}")
        try: raw.close()
        except Exception: pass
        return
    if FB.local():                                 # 단독 모드이거나 폴백 중: 허브가 직접 받는다
        return broker(down, addr)
    try:
        up = cloud_tls()
    except Exception as e:
        log(f"{addr[0]} 클라우드 쪽 실패: {e}")
        if FB.fail(_why_of(e)):                    # 두 번째 실패(또는 이미 폴백 중): 이 연결부터 직접 받는다
            return broker(down, addr)
        down.close(); return
    log(f"{addr[0]} bridged  module({down.version()}) <-> cloud({up.version()})")
    relay(down, up, addr)


def relay(down, up, addr):
    """기기(down) ↔ 클라우드(up) 중계 — 한 스레드가 poll 로 양쪽을 다 읽고 쓴다(펌웨어 hub_proxy.c 와 같은 구조).
    파이썬 SSL 소켓은 한 객체를 두 스레드가 동시에 읽고 쓰면 TLS 기록이 깨질 수 있어(실측: 클라우드 쪽
    'wrong version number') 스레드를 나누지 않는다. 클라우드→기기는 다 온 패킷만 보내므로 기기 쪽 흐름은 늘 패킷
    경계 — 그 사이에 명령(주입 대기열)을 끼워 넣는다. 대기열은 0.3초마다 확인한다."""
    down.settimeout(RELAY_READ_SEC); up.settimeout(RELAY_READ_SEC)   # poll 뒤 읽기가 오래 매달리지 않게(쓰기도 이 제한)
    link = None; on_pub = None
    pending = []; plock = threading.Lock()
    if CORE is not None:
        def inject(pkt):
            with plock:
                if len(pending) >= MAX_PENDING:
                    log(f"{addr[0]} 주입 대기 꽉 참 — 버림"); return
                pending.append(pkt)
        link = CORE.link_open(inject)
        def on_pub(topic, payload): CORE.on_device_publish(link, topic, payload)
    bm = bc = b""; last = time.monotonic(); why = "?"; sel = None
    gen = FB.gen
    t_login = None            # 기기의 로그인(CONNECT)을 서버로 넘긴 때
    accepted = False          # 서버가 그 로그인을 받아 줌(CONNACK 코드 0) — 여기까지 와야 '성공'
    up_why = up_rc = 0
    try:
        sel = Selector()
        sel.register(down, selectors.EVENT_READ); sel.register(up, selectors.EVENT_READ)
        while True:
            if FB.gen != gen:
                why = "동작 방식 바뀜"; break
            if t_login is not None and not accepted and time.monotonic() - t_login > ACCEPT_WAIT_S:
                why = f"서버가 로그인에 {ACCEPT_WAIT_S}초째 답이 없음"; up_why = WHY_NOREPLY; break
            with plock:
                q, pending[:] = pending[:], []
            for pkt in q:
                down.sendall(pkt)
                log(f"{addr[0]} 명령 주입 {len(pkt)}B")
            ready = [s for s in (down, up) if s.pending()]   # TLS 가 이미 풀어 둔 데이터는 소켓에 안 보인다 — 먼저 확인
            if not ready:
                ready = [k.fileobj for k, _ in sel.select(0.3)]
                if not ready:
                    if time.monotonic() - last > IDLE_SEC:
                        why = "유휴"; break
                    continue
            last = time.monotonic()
            for s in ready:
                try:
                    d = s.recv(4096)
                except (socket.timeout, ssl.SSLWantReadError):
                    continue                                 # 앱 데이터 없는 기록(티켓 등)·덜 온 기록: 다음 바퀴에
                if not d:
                    why = "기기가 닫음" if s is down else "클라우드가 닫음"; raise EOFError
                if s is down:                                # 기기 → 클라우드: 그대로 넘기고 엿보기
                    up.sendall(d)
                    if t_login is None and not bm and d[0] >> 4 == 1:
                        t_login = time.monotonic()
                    bm = parse(bm + d, "M->C", on_pub)
                else:                                        # 클라우드 → 기기: 다 온 패킷만(주입 경계 유지)
                    bc += d
                    left = parse(bc, "C->M")
                    whole = bc[:len(bc) - len(left)]; bc = left
                    if whole:
                        down.sendall(whole)
                        if not accepted and whole[0] >> 4 == 2 and len(whole) >= 4:   # 서버의 답(CONNACK): 넷째 바이트가 코드
                            if whole[3] == 0:
                                accepted = True
                                FB.accepted(+1)
                            else:
                                up_why, up_rc = WHY_REFUSED, whole[3]
                                why = f"서버가 로그인을 거절함(코드 {up_rc})"; raise EOFError
    except EOFError:
        pass
    except Exception as e:
        why = f"{type(e).__name__}:{e}"
    finally:
        if link is not None:
            CORE.link_close(link)
        for s in (sel, down, up):
            try: s.close()
            except Exception: pass
        log(f"{addr[0]} closed ({why})")
        if accepted:
            FB.accepted(-1)
        elif FB.gen == gen:
            # 로그인을 받아 주기 전에 끝났다. 서버가 끊었으면 그대로 실패, 기기가 먼저 포기했으면 로그인을 보내고
            # 3초 넘게 기다린 경우만 '답이 없음'으로 센다(기기 쪽 Wi-Fi 가 잠깐 끊긴 것까지 서버 탓으로 세지 않게)
            if not up_why and why == "클라우드가 닫음":
                up_why = WHY_CLOSED
            elif not up_why and why == "기기가 닫음" and t_login is not None and time.monotonic() - t_login > 3:
                up_why = WHY_NOREPLY
            if up_why:
                FB.fail(up_why, up_rc, after_login=True)   # 이 연결은 닫혔다 — 기기가 다시 붙을 때 직접 받는다

# ---------------------------------------------------------------- 단독 모드: 허브가 MQTT 상대
def _reply(b0, body):
    """기기 → 허브 패킷 하나에 대한 답(없으면 b""), 닫아야 하면 None. ESP32 broker_reply 와 같은 최소 동작."""
    t = b0 >> 4
    if t == 1:                                   # CONNECT → CONNACK(수락)
        return bytes([0x20, 2, 0, 0])
    if t == 3:                                   # PUBLISH: QoS1 → PUBACK, QoS2 → PUBREC
        qos = (b0 >> 1) & 3
        tl = (body[0] << 8) | body[1]
        if qos and len(body) >= 2 + tl + 2:
            return bytes([0x40 if qos == 1 else 0x50, 2]) + body[2 + tl:4 + tl]
        return b""
    if t == 6 and len(body) >= 2:                # PUBREL → PUBCOMP
        return bytes([0x70, 2]) + body[:2]
    if t == 8 and len(body) >= 2:                # SUBSCRIBE → SUBACK(QoS1 까지)
        granted, pos = [], 2
        while pos + 2 <= len(body):
            tl = (body[pos] << 8) | body[pos + 1]; pos += 2 + tl
            if pos >= len(body): break
            granted.append(min(body[pos], 1)); pos += 1
        return bytes([0x90, 2 + len(granted)]) + body[:2] + bytes(granted)
    if t == 10 and len(body) >= 2:               # UNSUBSCRIBE → UNSUBACK
        return bytes([0xB0, 2]) + body[:2]
    if t == 12:                                  # PINGREQ → PINGRESP
        return bytes([0xD0, 0])
    if t == 14:                                  # DISCONNECT
        return None
    return b""                                   # PUBACK 등(우리 명령에 대한 답)

def _topics(t, body):
    """SUBSCRIBE 의 토픽들 / PUBLISH 의 (토픽, payload)."""
    if t == 8:
        out, pos = [], 2
        while pos + 2 <= len(body):
            tl = (body[pos] << 8) | body[pos + 1]
            out.append(body[pos + 2:pos + 2 + tl].decode("utf-8", "replace")); pos += 2 + tl + 1
        return out
    return []

def broker(down, addr):
    link = None; pending = []; plock = threading.Lock()
    if CORE is not None:
        def inject(pkt):
            with plock:
                if len(pending) < MAX_PENDING: pending.append(pkt)
        link = CORE.link_open(inject)
    log(f"{addr[0]} 연결됨: 기기({down.version()}) <-> 허브({'단독 모드' if STANDALONE else '서버에 닿지 않아 직접 받는 중'})")
    down.settimeout(0.3)
    buf = b""; last = time.time(); why = "?"
    gen = FB.gen
    try:
        while True:
            if FB.gen != gen:                    # 폴백이 끝남 → 끊어서 기기가 프록시로 다시 붙게
                why = "동작 방식 바뀜"; break
            with plock:
                q, pending[:] = pending[:], []
            for pkt in q:
                down.sendall(pkt)
                log(f"{addr[0]} 기기로 보냄 {len(pkt)}B")
            try:
                d = down.recv(4096)
            except (socket.timeout, ssl.SSLWantReadError):
                if time.time() - last > IDLE_SEC: why = "유휴"; break
                continue
            if not d: why = "기기가 닫음"; break
            last = time.time(); buf += d
            while True:                          # 다 모인 패킷마다(길이 필드 깨짐·MAX_PKT 초과는 FrameError 로 끊음)
                f = frame(buf, 0)
                if f is None: break
                b0, j, end = f
                body, buf = buf[j:end], buf[end:]
                t = b0 >> 4
                if t == 3 and CORE is not None:
                    tl = (body[0] << 8) | body[1]
                    topic = body[2:2 + tl].decode("utf-8", "replace")
                    rest = body[2 + tl + (2 if (b0 >> 1) & 3 else 0):]
                    log(f"{addr[0]} 기기→허브 PUBLISH {topic.rsplit('/', 1)[-1][:40]} {len(rest)}B")
                    try: CORE.on_device_publish(link, topic, rest)
                    except Exception as e: log(f"{addr[0]} 상태 해석 오류: {e}")
                r = _reply(b0, body)
                if r is None: why = "DISCONNECT"; raise StopIteration
                if r: down.sendall(r)
                if t == 1: log(f"{addr[0]} CONNECT → CONNACK")
                if t == 8:
                    for tp in _topics(t, body):
                        log(f"{addr[0]} SUBSCRIBE {tp[:200]} → SUBACK")
                        if CORE is not None:
                            try: CORE.on_device_subscribe(link, tp)
                            except Exception as e: log(f"{addr[0]} 구독 처리 오류: {e}")
    except StopIteration:
        pass
    except Exception as e:
        why = f"{type(e).__name__}:{e}"
    finally:
        if link is not None: CORE.link_close(link)
        try: down.close()
        except Exception: pass
        log(f"{addr[0]} 연결 종료 ({why})")

class Slots:
    """출발지별 동시 연결 수(:8883 과 hub_api 의 :80 이 하나씩 쓴다). 전체 상한(total)은 등록 안 된 출발지끼리만 나눠 쓴다 —
    등록된 기기(RINNAI_IPS·승인한 기기)는 따로 세므로, 다른 출발지가 슬롯을 다 잡아도 기기의 재접속은 받는다(기기도
    출발지당 per_ip 는 지킨다). RINNAI_IPS=any 면 기기를 가려낼 수 없어 모두 전체 상한을 같이 쓴다."""
    def __init__(self, total, per_ip):
        self.total, self.per_ip = total, per_ip
        self.n = {}; self.lock = threading.Lock()

    def take(self, ip):
        with self.lock:
            if self.n.get(ip, 0) >= self.per_ip: return False
            if not _apart(ip) and sum(v for k, v in self.n.items() if not _apart(k)) >= self.total: return False
            self.n[ip] = self.n.get(ip, 0) + 1; return True

    def give(self, ip):
        with self.lock:
            self.n[ip] -= 1
            if not self.n[ip]: del self.n[ip]

_conn = Slots(MAX_CONN, MAX_PER_IP)
_slots, _take, _give = _conn.n, _conn.take, _conn.give

def _serve(c, a):
    try: handle(c, a)
    except Exception as e: log(f"{a[0]} 처리 오류: {type(e).__name__}:{e}")
    finally:
        try: c.close()
        except Exception: pass
        _give(a[0])

def run(core=None):
    global CORE
    CORE = core
    if DEVICE_ERR:                                   # nas_hub 가 먼저 걸러 주지만 이 파일만 돌릴 때도 잘못된 값으로 뜨지 않게
        log(f"오류: {DEVICE_ERR}"); raise SystemExit(2)
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(LISTEN); s.listen(16)
    if not STANDALONE and not UP_CA:
        log("오류: UPSTREAM_CA 가 비어 있음 — 검증 없이는 클라우드에 붙지 않음")
    elif not STANDALONE and not os.path.exists(UP_CA):
        log(f"오류: UPSTREAM_CA 파일 없음({UP_CA}) — 클라우드에 붙지 않음")
    log(f"기기 대기 :{LISTEN[1]}  {'단독 모드(클라우드 없음)' if STANDALONE else '프록시 -> ' + (', '.join(upstream.ips(log)) or '(주소 아직 모름)') + ':' + str(UP_PORT)}"
        f"{'' if STANDALONE else '  자동 폴백 ' + ('켬' if FB.on else '끔')}"
        f"  대상={'모든 출발지(any)' if DEVICE_ANY else sorted(DEVICE_IP)}"
        f"{'' if REGISTRY is None else ' + 승인한 기기 %d대' % len(REGISTRY.snapshot()['registered'])}"
        f"  동시 연결 {MAX_CONN}(출발지당 {MAX_PER_IP}, 등록된 기기는 따로)")
    if DEVICE_ANY:
        log("경고: RINNAI_IPS=any — 집 LAN 의 아무 기기나 린나이 기기로 받아들임(가짜 상태·명령 가로채기 가능)")
    while True:
        try:
            c, a = s.accept()
        except OSError as e:                         # 파일 핸들 부족 등: 죽지 말고 잠깐 쉬었다 계속
            log(f"accept 오류: {e}"); time.sleep(1); continue
        if not _take(a[0]):
            log(f"{a[0]} 연결 상한 — 거절")
            c.close(); continue
        try:
            threading.Thread(target=_serve, args=(c, a), daemon=True).start()
        except RuntimeError as e:
            log(f"{a[0]} 스레드 못 만듦: {e}"); c.close(); _give(a[0])

if __name__ == "__main__":
    run()
