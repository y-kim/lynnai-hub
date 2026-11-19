"""
hub_rinnai.py (대응 ESP32: hub_rinnai.c + hub_uplink.c) -- 중계되는 MQTT 를 엿봐 보일러(f06)·각방(f09) 상태를 모으고,
SmartThings 명령을 기기 쪽 MQTT 로 조립한다. ESP32 펌웨어 hub_rinnai.c + hub_uplink.c 의 파이썬 판.

메시지 번호·형식은 펌웨어 hub_uplink.h 와 같다(LAN 드라이버 core.lua 가 그대로 받는다):
  허브 → 앱   0x01 상태 [종류 6|9][EPC70 블롭]   0x02 연결 [bit0 보일러, bit1 각방]
  앱 → 허브   0x00 명령 "f06 b1=1" / "f09 a0=7,b1=1,d4=10" / "refresh"(여러 개 ";")
NAS 엔 허브에 꽂는 센서(온습도계·가스)가 없으므로 0x03~0x06 은 쓰지 않는다.
"""
import re
import struct
import threading
import time

CMD_REQUEST, CMD_STATE, CMD_LINKS, CMD_PAIRS = 0x00, 0x01, 0x02, 0x07
KIND_F06, KIND_F09 = 6, 9
HEARTBEAT_S = 300
_STATE_RE = re.compile(rb'"id":"70","data":"([0-9a-fA-F]+)"')
_TOK = re.compile(r"[0-9a-z]{1,2}")
_VAL = re.compile(r"[0-9a-z]{1,16}")


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
    """base 토픽 끝 세그먼트(MAC "34:5f:45:00:00:01")에서 끝 3바이트."""
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
    def __init__(self, log=print):
        self.log = log
        self.lock = threading.Lock()
        self.links = []
        self.state = {}       # (kind, mac3) -> blob(bytes)
        self.senders = []     # 내보내기 대상(LAN uplink 등): fn(cmd_id, data_bytes)
        self._last_mask = None
        threading.Thread(target=self._heartbeat, daemon=True).start()

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

    def on_device_publish(self, ln, topic, payload):
        """기기 → 클라우드 PUBLISH. 종류·상태 블롭을 뽑는다."""
        seg = topic.rsplit("/", 1)
        kind = KIND_F06 if "/f06/" in topic else KIND_F09 if "/f09/" in topic else 0
        if kind and ln.kind != kind:
            ln.kind, ln.base = kind, seg[0]
            ln.mac3 = _mac3(seg[0])
            self.log(f"[core] 링크 = f0{kind} {ln.mac3.hex()} ({ln.base})")
            self._send_links()
        if len(seg) < 2 or seg[1] not in ("inf", "res") or not kind or len(ln.mac3) != 3:
            return
        m = _STATE_RE.search(payload)
        if not m:
            return
        blob = bytes.fromhex(m.group(1).decode()[:128])
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
            with self.lock:
                items = list(self.state.items())
            self._send_links()
            for (kind, mac), blob in items:
                self._emit(CMD_STATE, bytes([kind]) + mac + blob)
