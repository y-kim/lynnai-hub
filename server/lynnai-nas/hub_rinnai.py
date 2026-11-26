"""
hub_rinnai.py (대응 ESP32: hub_rinnai.c + hub_uplink.c) -- 중계되는 MQTT 를 엿봐 보일러(f06)·각방(f09) 상태를 모으고,
SmartThings 명령을 기기 쪽 MQTT 로 조립한다. ESP32 펌웨어 hub_rinnai.c + hub_uplink.c 의 파이썬 판.

메시지 번호·형식은 펌웨어 hub_uplink.h 와 같다(LAN 드라이버 core.lua 가 그대로 받는다):
  허브 → 앱   0x01 상태 [종류 6|9][EPC70 블롭]   0x02 연결 [bit0 보일러, bit1 각방]
  앱 → 허브   0x00 명령 "f06 b1=1" / "f09 a0=7,b1=1,d4=10" / "refresh"(여러 개 ";")
NAS 엔 허브에 꽂는 센서(온습도계·가스)가 없으므로 0x03~0x06 은 쓰지 않는다.

단독 모드(standalone=True, 클라우드 없음)에서는 클라우드가 하던 일을 코어가 한다(ESP32 와 같음):
  - 기기가 <base>/get 을 구독하면 곧바로 상태 요청(fa)
30초마다 상태 요청(fa)은 두 모드 다 한다(ESP32 poll_status 와 같음 — 보일러가 상태를 자주 안 올려 외출 등을 빨리 반영).
  - 기기의 시간 요청(<base>/stg, 약 1시간마다)에 <base>/set 72 = 현재 한국 시각으로 답함

명령 확인·재전송(ESP32 hub_rinnai.c 와 같은 규칙): 명령이 바꾸려던 칸을 그 뒤 상태 보고(70)에서 보고, 2.5초 안에
나타나지 않으면 같은 명령을 다시 보낸다(모두 3번). 칸을 모르는 명령은 보낸 지 0.3초 뒤의 첫 보고를 답으로 친다.
온수 예열 자동 끄기: 켜진 것을 본 지 정한 분이 지나면 허브가 b6=0 을 보낸다(0 = 무제한). 설정은 DATA_DIR/boiler.json,
바꾸기는 "hub preoff=<분>", 알림은 0x0c [분].
"""
import json
import os
import re
import struct
import threading
import time

CMD_REQUEST, CMD_STATE, CMD_LINKS, CMD_PAIRS = 0x00, 0x01, 0x02, 0x07
CMD_MODELS = 0x0D               # 모델코드: ([종류][MAC끝3][모델코드 1바이트 = 토픽의 두 자리 16진수]) * n — 드라이버가 온도 표를 고른다
CMD_LABELS = 0x0E               # 허브 라벨: ([종류][MAC끝3][len][UTF-8 라벨]) * n — 그 기기를 품은 허브 라벨(층). 방 이름 앞에 붙인다
_MODEL = re.compile(r"/f0[69]/([0-9A-Fa-f]{2})/")
CMD_BOILCFG = 0x0C              # 보일러 설정: [온수 예열 자동 끄기(분, 0 = 무제한)][보일러 모드 0 정하지 않음/1 난방수/2 온돌·실내]
BOILER_MODES = {"": 0, "water": 1, "room": 2}   # 환경변수 BOILER_MODE (ESP32 는 초기 설정·관리 화면에서 고름)
OK_CODES = {"80", "b1", "b3", "b4", "b6", "d1", "d7", "df", "d2", "d4", "a0", "96", "97", "da"}   # ESP32 code_allowed 와 같게
PREOFF_CHOICES = (*range(1, 61), 0)        # 1~60분, 0 = 무제한
PEND_WAIT_S, PEND_TRIES, PEND_ANY_S = 2.5, 3, 0.3
PEND_REVERT_S = 25          # 반영이 확인된 뒤에도 이만큼 '되돌림'을 지켜본다(제어기가 ~20초 뒤 원래 값으로 되돌림) — 기록만, 재전송 안 함
# 확인할 칸: f06 = (니블 위치, 두 니블 값인가), f09 = 방(a0) stat 바이트 기준의 (비트 | 바이트 오프셋)
F06_CHECK = {"80": (0, False), "b1": (2, False), "d7": (3, False), "b6": (5, False), "b4": (7, False),
             "d1": (10, True), "d2": (13, True), "d4": (15, True)}
F09_BIT = {"b1": 0x02, "b3": 0x10, "96": 0x08, "b4": 0x04}   # b4 = 시간 모드(2026-11-23 앱 캡처)
F09_BYTE = {"97": 1, "d4": 4, "da": 5}   # da = 시간 난방 분(1시간에 N분)


def _nib(b, i):
    return (b[i // 2] & 0xF) if i % 2 else (b[i // 2] >> 4)


def build_checks(kind, items):
    """[(code, value_int)] → (확인 함수 목록, 칸 이름). 함수는 상태 블롭을 받아 그 칸이 명령 값인지 돌려준다."""
    checks, key, zone = [], [], None
    for c, v in items:
        if kind == KIND_F09 and c == "a0" and 1 <= v <= 7:
            zone = v - 1
        if c != "97":                                   # 예약 시간은 칸 이름에 넣지 않는다(걸기·풀기가 같은 칸)
            key.append("a0=%x" % v if c == "a0" else c)
    for c, v in items:
        if kind == KIND_F06 and c in F06_CHECK:
            pos, two = F06_CHECK[c]
            if two:
                checks.append(lambda b, p=pos, w=v: len(b) * 2 > p + 1 and ((_nib(b, p) << 4) | _nib(b, p + 1)) == w)
            else:
                checks.append(lambda b, p=pos, w=v: len(b) * 2 > p and (_nib(b, p) != 0) == (w != 0))
        elif kind == KIND_F09 and zone is not None:
            st = zone * 7
            if c in F09_BIT:
                bit = F09_BIT[c]
                checks.append(lambda b, s=st, m=bit, w=v: len(b) > s and (b[s] & m) == (m if w else 0))
            elif c in F09_BYTE:
                checks.append(lambda b, s=st + F09_BYTE[c], w=v: len(b) > s and b[s] == w)
    return checks[:3], ",".join(key)


class Pending:
    def __init__(self, kind, mac3, ln, pkt, checks, key, src):
        self.kind, self.mac3, self.ln, self.pkt, self.checks, self.key, self.src = kind, mac3, ln, pkt, checks, key, src
        self.tries, self.sent = 1, time.monotonic()
        self.ok = 0.0            # 반영이 확인된 때(0 = 아직 확인 전, >0 = 되돌림 감시 중)
KIND_F06, KIND_F09 = 6, 9
HEARTBEAT_S = 300
POLL_S = 30
MAX_STATE = 16                 # 상태표 항목 상한(실제는 보일러 + 조절기 몇 대). 넘으면 끊긴 기기의 것부터 버린다
_STATE_RE = re.compile(rb'"id":"70","data":"([0-9a-fA-F]+)"')
_TOK = re.compile(r"[0-9a-z]{1,2}")
_VAL = re.compile(r"[0-9a-z]{1,16}")
_TOPIC = re.compile(r"[\x21-\x7e]{1,128}")   # 배울 토픽: 짧은 출력 가능 ASCII 만(깨진 UTF-8·긴 토픽은 무시). '+','#' 는 아래서


def _mqtt_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def mqtt_publish(topic, payload):
    """QoS0 PUBLISH 패킷."""
    t = topic.encode()
    body = struct.pack(">H", len(t)) + t + payload
    return bytes([0x30]) + _mqtt_len(len(body)) + body


def _mac3(base):
    """base 토픽 끝 세그먼트(MAC "34:5F:45:00:00:01" 꼴)에서 끝 3바이트."""
    seg = base.rsplit("/", 1)[-1]
    try:
        b = bytes(int(x, 16) for x in seg.split(":"))
        return b[-3:] if len(b) >= 3 else b""
    except ValueError:
        return b""


class Link:
    """기기 하나의 중계 연결. relay 가 만든다. 명령은 inject_cb(패킷) 로 기기 쪽에 써 넣는다."""
    def __init__(self, inject_cb):
        self.inject_cb = inject_cb
        self.kind = 0
        self.base = ""        # rinnai/RK/01/RK/f06/23/<MAC>
        self.mac3 = b""       # MAC 끝 3바이트 = instance


class HubCore:
    def __init__(self, log=print, standalone=False, data_dir=None, boiler_mode=""):
        self.log = log
        self.boiler_mode = BOILER_MODES.get(boiler_mode, 0)   # 보일러 Wi-Fi 하나만 달린 집은 조절기가 있는지 알 길이 없어 설치 때 정한다
        if boiler_mode not in BOILER_MODES:
            log(f"[core] 모르는 BOILER_MODE={boiler_mode!r} — water/room 중 하나. 정하지 않음으로")
        self.data_dir = data_dir
        self.pending = []                 # 확인을 기다리는 명령(Pending)
        self.counts = {"tracked": 0, "resent": 0, "failed": 0, "reverted": 0}
        self.last_fail = ""
        self.last_revert = ""
        self.preheat_off = self._load_preoff()
        self.pre_on = {}                  # (kind, mac3) -> 온수 예열이 켜진 것을 처음 본 때(monotonic)
        self.pre_try = {}                 # (kind, mac3) -> 자동 끄기를 마지막으로 보낸 때
        self.pre_off_at = {}              # (kind, mac3) -> 이때 끈다(monotonic, None = 무제한). 켜진 동안 고르면 다시 정함
        self._pre_dirty = False           # 예열이 켜지거나 꺼짐 → 남은 시간을 바로 알림
        self._pre_beat = 0.0
        self._standalone = standalone     # 참/거짓, 또는 지금 허브가 직접 받는 중인지 알려 주는 함수(자동 폴백)
        self.hub_request = None           # "hub …" 요청을 받을 함수(nas_hub 가 hub_proxy.request 를 넣는다)
        self.on_resend = None             # 새로고침 때 덧붙여 보낼 것(동작 모드)
        self.lock = threading.Lock()
        self.links = []
        self.state = {}       # (kind, mac3) -> blob(bytes)
        self.models = {}      # (kind, mac3) -> 모델코드(토픽의 두 자리 16진수) — 온수·난방수 온도 표를 가른다
        self.label = os.environ.get("LYNNAI_LABEL", "").strip()[:24]   # 이 허브 라벨(층) — 방 이름 앞에 붙인다(ESP32 hub v0.9 와 같은 동작)
        self.senders = []     # 내보내기 대상(LAN uplink 등): fn(cmd_id, data_bytes)
        self._last_mask = None
        threading.Thread(target=self._heartbeat, daemon=True).start()
        threading.Thread(target=self._poll_loop, daemon=True).start()   # 두 모드 다(ESP32 와 같음)
        threading.Thread(target=self._pend_loop, daemon=True).start()   # 명령 확인·재전송 + 예열 자동 끄기

    @property
    def standalone(self):
        """허브가 클라우드 노릇을 해야 하나(단독 모드, 또는 프록시인데 서버에 닿지 않아 직접 받는 중)."""
        s = self._standalone
        return bool(s() if callable(s) else s)

    def emit(self, cmd, data):
        """스마트홈으로 한 건 보낸다(동작 모드 알림용)."""
        self._emit(cmd, data)

    # ---- 내보내기 대상 등록(LAN uplink) ----
    def add_sender(self, fn):
        with self.lock:
            self.senders.append(fn)

    def remove_sender(self, fn):
        with self.lock:
            if fn in self.senders:
                self.senders.remove(fn)

    def _emit(self, cmd_id, data):
        for fn in list(self.senders):
            try:
                fn(cmd_id, data)
            except Exception as e:
                self.log(f"[core] sender 오류: {e}")

    def _links_blob(self):
        out = b""
        for ln in self.links:
            if ln.kind and len(ln.mac3) == 3:
                out += bytes([ln.kind]) + ln.mac3
        return out

    def _models_blob(self):
        out = b""
        for ln in self.links:
            if ln.kind and len(ln.mac3) == 3:
                m = self.models.get((ln.kind, bytes(ln.mac3)))
                if m is not None:
                    out += bytes([ln.kind]) + ln.mac3 + bytes([m])
        return out

    def _labels_blob(self):
        if not self.label:
            return b""
        lb = self.label.encode("utf-8")[:24]
        out = b""
        for ln in self.links:
            if ln.kind and len(ln.mac3) == 3:
                out += bytes([ln.kind]) + ln.mac3 + bytes([len(lb)]) + lb
        return out

    def model_of(self, kind, mac_hex):
        """모델코드 두 자리("23") — 모르면 None. 연결이 끊겨도 기억한다."""
        m = self.models.get((kind, bytes.fromhex(mac_hex)))
        return None if m is None else f"{m:02x}"

    def label_of(self, kind, mac_hex):
        """그 기기를 품은 허브 라벨(층) — 없으면 ''. NAS 는 단일 버스라 모든 기기가 같은 라벨."""
        return self.label

    def _send_links(self):
        b = self._links_blob()
        if b != self._last_mask:
            self._last_mask = b
            self.log(f"[core] 연결 {len(b)//4}대")
        lb = self._labels_blob()
        if lb:
            self._emit(CMD_LABELS, lb)    # 라벨 먼저: 방 자식기기 생성 때 이름에 붙게(옛 드라이버는 무시)
        self._emit(CMD_LINKS, b)
        m = self._models_blob()
        if m:
            self._emit(CMD_MODELS, m)     # 옛 드라이버는 모르는 메시지를 무시한다
        self._emit(CMD_PAIRS, b"")        # NAS 는 수동 짝 저장 없음 → 빈 목록(드라이버 암묵 연동)

    # ---- relay 가 부른다 ----
    def link_open(self, inject_cb):
        ln = Link(inject_cb)
        with self.lock:
            self.links.append(ln)
        return ln

    def link_close(self, ln):
        with self.lock:
            if ln in self.links:
                self.links.remove(ln)
        self._send_links()

    def _learn(self, ln, topic):
        """이 링크의 기기 토픽(base = 마지막 '/' 앞)과 종류를 배운다."""
        if not _TOPIC.fullmatch(topic) or "+" in topic or "#" in topic:
            return 0
        seg = topic.rsplit("/", 1)
        kind = KIND_F06 if "/f06/" in topic else KIND_F09 if "/f09/" in topic else 0
        if kind and ln.kind != kind:
            ln.kind, ln.base = kind, seg[0]
            ln.mac3 = _mac3(seg[0])
            mm = _MODEL.search(seg[0] + "/")
            if mm and len(ln.mac3) == 3:
                self.models[(kind, bytes(ln.mac3))] = int(mm.group(1), 16)
            self.log(f"[core] 링크 = f0{kind} {ln.mac3.hex()} ({ln.base})")
            self._send_links()
        return kind

    def on_device_subscribe(self, ln, topic):
        """기기의 SUBSCRIBE(<base>/set, <base>/get) — 접속하자마자 토픽을 배운다. 단독이면 바로 상태 요청."""
        seg = topic.rsplit("/", 1)
        if len(seg) < 2 or seg[1] not in ("set", "get"):
            return
        self._learn(ln, topic)
        if self.standalone and seg[1] == "get":
            self.poll(ln)

    def poll(self, ln):
        """상태 요청(fa) — 클라우드가 하던 것과 같은 모양."""
        if not ln.kind or not ln.base:
            return
        body = ('{"ptn":"J07","code":"null","id":"f0%d","no":"1","sum":1,"rpc":[{"id":"fa"}],"lt":"null"}'
                % ln.kind).encode()
        ln.inject_cb(mqtt_publish(ln.base + "/get", body))

    def _poll_loop(self):
        while True:
            time.sleep(POLL_S)
            with self.lock:
                links = list(self.links)
            for ln in links:
                try:                                  # 링크 하나의 오류로 이 스레드가 죽지 않게
                    self.poll(ln)
                except Exception as e:
                    self.log(f"[core] 상태 요청 오류: {e}")

    def _answer_time(self, ln):
        """단독: 기기의 시간 요청(stg)에 set 72 = 현재 한국 시각(YYYYMMDDTHHMMSS)."""
        now = time.time()
        if now < 1704067200:                 # 시계가 안 맞으면 틀린 시각은 안 준다
            self.log("[core] 단독: 시간 요청 — 시계가 안 맞아 건너뜀")
            return
        ts = time.strftime("%Y%m%dT%H%M%S", time.gmtime(now + 9 * 3600))
        body = ('{"ptn":"J07","code":"null","id":"f0%d","no":"1","sum":1,"rpc":[{"id":"72","data":"%s"}],"lt":"null"}'
                % (ln.kind, ts)).encode()
        ln.inject_cb(mqtt_publish(ln.base + "/set", body))
        self.log(f"[core] 단독: f0{ln.kind} 시간 알려 줌 {ts}")

    def on_device_publish(self, ln, topic, payload):
        """기기 → 클라우드 PUBLISH. 종류·상태 블롭을 뽑는다(단독이면 시간 요청에도 답한다)."""
        seg = topic.rsplit("/", 1)
        kind = self._learn(ln, topic)
        if len(seg) == 2 and seg[1] == "stg" and self.standalone and ln.kind:
            self._answer_time(ln)
            return
        if len(seg) < 2 or seg[1] not in ("inf", "res") or not kind or len(ln.mac3) != 3:
            return
        m = _STATE_RE.search(payload)
        if not m:
            return
        hx = m.group(1).decode()[:128]
        blob = bytes.fromhex(hx[:len(hx) & ~1])           # 홀수 자리면 마지막 자리 버림
        key = (kind, bytes(ln.mac3))
        self._pend_report(kind, bytes(ln.mac3), blob)
        if kind == KIND_F06 and len(blob) > 2:              # 온수 예열이 켜진 때를 기억(자동 끄기)
            if _nib(blob, 5) != 1:
                if self.pre_on.pop(key, None) is not None:
                    self._pre_dirty = True
                self.pre_off_at.pop(key, None)
            elif key not in self.pre_on:
                now = time.monotonic()
                self.pre_on[key] = now
                self.pre_off_at[key] = now + self.preheat_off * 60 if self.preheat_off else None   # 기본값으로
                self.pre_try.pop(key, None)
                self._pre_dirty = True
        with self.lock:
            changed = self.state.get(key) != blob
            if key not in self.state:
                self._make_room()
            self.state[key] = blob
        if changed and blob:
            self.log(f"[core] f0{kind} {ln.mac3.hex()} 상태 {blob.hex()}")
            self._emit(CMD_STATE, bytes([kind]) + ln.mac3 + blob)

    def _make_room(self):
        """상태표가 꽉 찼으면 한 칸 비운다(lock 을 쥔 채 부른다). 링크가 토픽을 바꿔 가며 올려도 표가 끝없이 늘지 않게 —
        지금 연결 안 된 기기의 것 중 가장 먼저 들어온 것부터, 다 연결돼 있으면 가장 먼저 들어온 것."""
        while len(self.state) >= MAX_STATE:
            live = {(ln.kind, bytes(ln.mac3)) for ln in self.links if ln.kind}
            old = next((k for k in self.state if k not in live), None) or next(iter(self.state))
            del self.state[old]
            self.log(f"[core] 상태표 꽉 참({MAX_STATE}) — f0{old[0]} {old[1].hex()} 버림")

    # ---- 앱에서 온 명령 ----
    def command(self, text):
        text = text.strip()
        if text == "refresh":
            self._resend()
            if self.on_resend:
                self.on_resend()
            return
        if text.startswith("hub preoff="):       # 보일러 설정: 온수 예열 자동 끄기(분)
            self.set_preheat_off(text[11:])
            return
        if text.startswith("hub "):              # 허브 자체에 대한 요청(동작 모드·자동 폴백) — 따로 온다
            if self.hub_request:
                self.hub_request(text[4:])
            return
        for one in text.split(";"):
            self._command_one(one.strip())

    def _command_one(self, s):
        m = re.match(r"^f0([69])(?:@([0-9a-f]{6}))? (.+)$", s)
        if not m:
            self.log(f"[core] 모르는 명령: {s}")
            return
        kind = int(m.group(1))
        target = bytes.fromhex(m.group(2)) if m.group(2) else None
        rpc, items = [], []
        for item in m.group(3).split(","):
            k, _, v = item.partition("=")
            k, v = k.strip(), v.strip()
            if not (_TOK.fullmatch(k) and _VAL.fullmatch(v)):
                self.log(f"[core] 잘못된 명령 항목: {item}")
                return
            if k not in OK_CODES:                     # 확인한 쓰기 코드만(ESP32 와 같음)
                self.log(f"[core] 허용 안 된 코드 {k} — 명령 버림")
                return
            rpc.append('{"id":"%s","data":"%s"}' % (k, v))
            try:
                items.append((k, int(v, 16)))
            except ValueError:
                items.append((k, -1))
        if not rpc:
            return
        ln = None
        with self.lock:
            for x in reversed(self.links):
                if x.kind == kind and (target is None or bytes(x.mac3) == target):
                    ln = x
                    break
        if not ln:
            self.log(f"[core] f0{kind} 연결 없음 — 명령 버림: {s}")
            return
        # 클라우드가 보내는 모양 그대로: sum = rpc 항목 수
        body = ('{"ptn":"J07","code":"null","id":"f0%d","no":"1","sum":%d,"rpc":[%s],"lt":"null"}'
                % (kind, len(rpc), ",".join(rpc))).encode()
        pkt = mqtt_publish(ln.base + "/set", body)
        self.log(f"[core] 명령 → {ln.base}/set {body.decode()}")
        checks, key = build_checks(kind, items)
        self._send_tracked(ln, pkt, checks, key, "smarthome")

    # ---- 명령 확인·재전송 ----
    def _send_tracked(self, ln, pkt, checks, key, src):
        mac = bytes(ln.mac3)
        with self.lock:
            self.pending = [q for q in self.pending if not (q.kind == ln.kind and q.mac3 == mac and q.key == key)]
            cur = self.state.get((ln.kind, mac))
            already = bool(checks) and cur is not None and all(c(cur) for c in checks)
            if not already:
                self.pending.append(Pending(ln.kind, mac, ln, pkt, checks, key, src))
                self.counts["tracked"] += 1
        ln.inject_cb(pkt)

    def _pend_report(self, kind, mac, blob):
        """상태 보고가 오면 기다리던 명령이 반영됐는지 / 반영된 뒤 되돌아갔는지 본다."""
        now = time.monotonic()
        with self.lock:
            keep = []
            for q in self.pending:
                mine = q.kind == kind and q.mac3 == mac
                if mine and not q.ok:                           # 아직 반영 확인 전
                    done = all(c(blob) for c in q.checks) if q.checks else now - q.sent >= PEND_ANY_S
                    if done:
                        if q.tries > 1:
                            self.log(f"[core] 명령 반영됨 — f0{kind} {mac.hex()} {q.key}({q.tries}번 보냄)")
                        if q.checks:                            # 칸을 아는 명령만 되돌림을 감시
                            q.ok = now
                            keep.append(q)
                        # 칸을 모르는 명령은 되돌림을 판정할 수 없다 — 여기서 끝낸다
                    else:
                        keep.append(q)
                elif mine and q.checks and not all(c(blob) for c in q.checks):   # 반영됐다 되돌아감(보일러가 안 받음)
                    self.counts["reverted"] += 1
                    self.last_revert = f"f0{q.kind} {q.key}"
                    self.log(f"[core] 명령이 반영됐다 {now - q.ok:.0f}초 뒤 되돌아감 — f0{q.kind} {q.mac3.hex()} {q.key}(보일러가 안 받음)")
                    self._emit_revert(q.kind, q.mac3, int(now - q.ok))
                else:
                    keep.append(q)
            self.pending = keep

    def _emit_revert(self, kind, mac3, secs):
        """되돌림을 이벤트로 알린다(허브 EV_CMD_REVERT 와 같은 뜻). 이벤트 수집기가 없으면 로그만."""
        for fn in getattr(self, "event_cbs", []):
            try:
                fn("cmd_revert", kind, mac3, secs)
            except Exception as e:
                self.log(f"[core] revert 이벤트 콜백 오류: {e}")

    def _pend_tick(self):
        now = time.monotonic()
        resend = []
        with self.lock:
            keep = []
            for q in self.pending:
                if q.ln not in self.links:                # 연결이 끊김 — 다시 붙으면 기기가 자기 상태를 올린다
                    continue
                if q.ok:                                  # 반영된 뒤 되돌림 감시 중 — 창이 지나면 '그대로 유지'로 보고 놓아준다
                    if now - q.ok < PEND_REVERT_S:        #   (되돌림은 _pend_report 가 상태 보고로 잡는다, 재전송 안 함)
                        keep.append(q)
                    continue
                if now - q.sent < PEND_WAIT_S:
                    keep.append(q)
                elif q.tries >= PEND_TRIES:
                    self.counts["failed"] += 1
                    self.last_fail = f"f0{q.kind} {q.key}"
                    self.log(f"[core] 명령이 반영되지 않음 — f0{q.kind} {q.mac3.hex()} {q.key}({q.tries}번 보냄, 포기)")
                else:
                    q.tries += 1
                    q.sent = now
                    self.counts["resent"] += 1
                    self.log(f"[core] 명령에 답이 없어 다시 보냄({q.tries}번째) — f0{q.kind} {q.mac3.hex()} {q.key}")
                    resend.append(q)
                    keep.append(q)
            self.pending = keep
        for q in resend:
            q.ln.inject_cb(q.pkt)

    def _preheat_tick(self):
        now = time.monotonic()
        if self._pre_dirty or (self.pre_on and now - self._pre_beat >= 60):   # 켜지고 꺼질 때, 켜진 동안 1분마다 남은 시간
            self._pre_dirty = False
            self._pre_beat = now
            self._send_boilcfg()
        for key, since in list(self.pre_on.items()):
            at = self.pre_off_at.get(key)
            if at is None or now < at or now - self.pre_try.get(key, -1e9) < 30:
                continue
            with self.lock:
                ln = next((x for x in reversed(self.links) if x.kind == key[0] and bytes(x.mac3) == key[1]), None)
            if not ln or not ln.base:
                continue
            self.pre_try[key] = now
            body = b'{"ptn":"J07","code":"null","id":"f06","no":"1","sum":1,"rpc":[{"id":"b6","data":"0"}],"lt":"null"}'
            self.log(f"[core] 온수 예열 자동 끄기(켜진 지 {int(now - since)}초) — f06 {key[1].hex()}")
            checks, ckey = build_checks(KIND_F06, [("b6", 0)])
            self._send_tracked(ln, mqtt_publish(ln.base + "/set", body), checks, ckey, "auto")

    def _pend_loop(self):
        while True:
            time.sleep(0.25)
            try:
                self._pend_tick()
                self._preheat_tick()
            except Exception as e:
                self.log(f"[core] 명령 확인 오류: {e}")

    # ---- 온수 예열 자동 끄기 설정 ----
    def _preoff_path(self):
        return os.path.join(self.data_dir, "boiler.json") if self.data_dir else None

    def _load_preoff(self):
        p = self._preoff_path()
        try:
            v = json.load(open(p))["preheat_off_min"] if p and os.path.exists(p) else 0
            return v if v in PREOFF_CHOICES else 0
        except Exception as e:
            self.log(f"[core] boiler.json 읽기 실패: {e} — 무제한으로")
            return 0

    def preheat_left(self):
        """0 = 예열 꺼짐, 1~60 = 꺼지기까지 남은 분(올림), 255 = 켜짐·무제한 (ESP32 와 같은 값)."""
        now = time.monotonic()
        for key in list(self.pre_on):
            if key[0] != KIND_F06:
                continue
            at = self.pre_off_at.get(key)
            if at is None:
                return 255
            return max(1, min(60, -int(-(at - now) // 60)))
        return 0

    def _boilcfg(self):
        """[자동 끄기 기본값(분)][보일러 모드][지금: 0 꺼짐 / 남은 분 / 255 무제한]"""
        return bytes([self.preheat_off, self.boiler_mode, self.preheat_left()])

    def _send_boilcfg(self):
        self._emit(CMD_BOILCFG, self._boilcfg())

    def set_preheat_off(self, text):
        """스마트홈의 "hub preoff=<분>". 예열이 켜져 있으면 이번 예열을 지금부터 그만큼(0 = 무제한), 꺼져 있으면 다음에 켜질 때의
        기본값을 바꾼다. 바뀌었든 아니든 지금 값을 다시 알린다(잘못된 값이면 화면이 되돌아간다)."""
        v = int(text) if str(text).isdigit() else -1
        running = [k for k in self.pre_on if k[0] == KIND_F06]
        if v in PREOFF_CHOICES and running:
            now = time.monotonic()
            for key in running:
                self.pre_off_at[key] = now + v * 60 if v else None
                self.pre_try.pop(key, None)
            self.log(f"[core] 이번 온수 예열: {f'{v}분 뒤 끔' if v else '무제한'}")
        elif v in PREOFF_CHOICES and v != self.preheat_off:
            p = self._preoff_path()
            try:
                if p:
                    tmp = p + ".tmp"
                    with open(tmp, "w") as f:
                        json.dump({"preheat_off_min": v}, f)
                    os.replace(tmp, p)
                self.preheat_off = v
                self.log(f"[core] 온수 예열 자동 끄기 → {v}분(0 = 무제한)")
            except OSError as e:
                self.log(f"[core] 온수 예열 자동 끄기 저장 실패: {e} — 그대로")
        elif v not in PREOFF_CHOICES:
            self.log(f"[core] 모르는 자동 끄기 값: {text[:8]}")
        self._send_boilcfg()

    def cmd_info(self):
        now = time.monotonic()
        with self.lock:
            on = [now - t for k, t in self.pre_on.items() if k[0] == KIND_F06]
            return {**self.counts, "pending": sum(1 for q in self.pending if not q.ok), "preheat_off_min": self.preheat_off,
                    "preheat_on_s": int(max(on)) + 1 if on else 0, "boiler_mode": self.boiler_mode,
                    "preheat_left_s": self._left_s(now)}

    def _left_s(self, now):
        for key in self.pre_on:
            if key[0] == KIND_F06:
                at = self.pre_off_at.get(key)
                return -1 if at is None else max(0, int(at - now))
        return 0

    # ---- 서버 API 용 ----
    def snapshot(self):
        """[(kind, mac3_hex, blob, linked), ...]"""
        with self.lock:
            live = {(ln.kind, bytes(ln.mac3)) for ln in self.links if ln.kind}
            return [(k, mac.hex(), blob, (k, mac) in live) for (k, mac), blob in self.state.items()]

    def _resend(self):
        """연결 목록 + 지금 연결된 기기의 상태만 다시 보낸다. 끊긴 기기의 마지막 상태는 보내지 않는다 —
        드라이버는 상태를 받으면 그 기기를 온라인으로 표시하므로(ESP32 hub_rinnai.c 의 주기 재전송과 같은 규칙)."""
        with self.lock:
            live = {(ln.kind, bytes(ln.mac3)) for ln in self.links if ln.kind}
            items = [(k, blob) for k, blob in self.state.items() if k in live and blob]
        self._send_links()
        self._send_boilcfg()                                               # 보일러 설정(자동 끄기·보일러 모드·남은 시간)
        for (kind, mac), blob in items:
            self._emit(CMD_STATE, bytes([kind]) + mac + blob)

    def _heartbeat(self):
        while True:
            time.sleep(HEARTBEAT_S)
            try:
                self._resend()
            except Exception as e:
                self.log(f"[core] 하트비트 오류: {e}")
