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
"""
import re
import struct
import threading
import time

CMD_REQUEST, CMD_STATE, CMD_LINKS, CMD_PAIRS = 0x00, 0x01, 0x02, 0x07
KIND_F06, KIND_F09 = 6, 9
HEARTBEAT_S = 300
POLL_S = 30
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
    def __init__(self, log=print, standalone=False):
        self.log = log
        self.standalone = standalone
        self.lock = threading.Lock()
        self.links = []
        self.state = {}       # (kind, mac3) -> blob(bytes)
        self.senders = []     # 내보내기 대상(LAN uplink 등): fn(cmd_id, data_bytes)
        self._last_mask = None
        threading.Thread(target=self._heartbeat, daemon=True).start()
        threading.Thread(target=self._poll_loop, daemon=True).start()   # 두 모드 다(ESP32 와 같음)

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

    def _send_links(self):
        b = self._links_blob()
        if b != self._last_mask:
            self._last_mask = b
            self.log(f"[core] 연결 {len(b)//4}대")
        self._emit(CMD_LINKS, b)
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
        with self.lock:
            changed = self.state.get(key) != blob
            self.state[key] = blob
        if changed and blob:
            self.log(f"[core] f0{kind} {ln.mac3.hex()} 상태 {blob.hex()}")
            self._emit(CMD_STATE, bytes([kind]) + ln.mac3 + blob)

    # ---- 앱에서 온 명령 ----
    def command(self, text):
        text = text.strip()
        if text == "refresh":
            with self.lock:
                items = list(self.state.items())
            self._send_links()
            for (kind, mac), blob in items:
                self._emit(CMD_STATE, bytes([kind]) + mac + blob)
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
        rpc = []
        for item in m.group(3).split(","):
            k, _, v = item.partition("=")
            k, v = k.strip(), v.strip()
            if not (_TOK.fullmatch(k) and _VAL.fullmatch(v)):
                self.log(f"[core] 잘못된 명령 항목: {item}")
                return
            rpc.append('{"id":"%s","data":"%s"}' % (k, v))
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
        ln.inject_cb(pkt)

    # ---- 서버 API 용 ----
    def snapshot(self):
        """[(kind, mac3_hex, blob, linked), ...]"""
        with self.lock:
            live = {(ln.kind, bytes(ln.mac3)) for ln in self.links if ln.kind}
            return [(k, mac.hex(), blob, (k, mac) in live) for (k, mac), blob in self.state.items()]

    def _heartbeat(self):
        while True:
            time.sleep(HEARTBEAT_S)
            try:
                with self.lock:
                    items = list(self.state.items())
                self._send_links()
                for (kind, mac), blob in items:
                    self._emit(CMD_STATE, bytes([kind]) + mac + blob)
            except Exception as e:
                self.log(f"[core] 하트비트 오류: {e}")
