"""
broker.py  --  최소 MQTT 브로커 상태머신 (순수 로직, Pico 포팅 대상)

transport를 모른다. "클라이언트당 세션 객체"가 들어온 바이트를 먹고(feed),
내보낼 바이트 묶음(outbox)을 돌려준다. TLS/소켓은 바깥(server_pi3.py)이 담당.

rkiot 흉내용으로 설계: 보일러가 CONNECT→SUBSCRIBE 하면 전부 accept 하고,
보일러가 올린 PUBLISH(상태 J07)는 on_publish 콜백으로 넘긴다.
우리가 보일러에 명령(J07)을 내릴 땐 Broker.publish_to(sub, topic, payload).
"""

import mqtt_proto as M


class Session:
    """한 TCP/TLS 연결 = 한 세션."""
    def __init__(self, broker, peer):
        self.broker = broker
        self.peer = peer
        self.inbuf = bytearray()
        self.outbox = bytearray()
        self.client_id = None
        self.subs = []              # [(topic_filter_bytes, qos), ...]
        self.connected = False
        self.alive = True

    # --- 바깥에서 소켓으로 받은 바이트를 넣는다 ---
    def feed(self, data):
        self.inbuf.extend(data)
        while True:
            pkt = M.take_packet(self.inbuf)
            if pkt is None:
                break
            self._handle(*pkt)

    # --- 바깥이 소켓으로 내보낼 바이트를 꺼낸다 ---
    def drain(self):
        if not self.outbox:
            return b""
        out = bytes(self.outbox)
        self.outbox = bytearray()
        return out

    def send(self, data):
        self.outbox.extend(data)

    def _handle(self, ptype, flags, payload):
        b = self.broker
        name = M.NAMES.get(ptype, "?%d" % ptype)
        if ptype == M.CONNECT:
            info = M.parse_connect(payload)
            self.client_id = info["client_id"]
            self.connected = True
            b.log("CONNECT", self.peer, "id=%r keepalive=%d user=%r" % (
                info["client_id"], info["keepalive"], info["user"]))
            # 전부 accept (code 0)
            self.send(M.connack(0, 0))
            if b.on_connect:
                b.on_connect(self, info)

        elif ptype == M.SUBSCRIBE:
            pid, topics = M.parse_subscribe(payload)
            granted = []
            for t, qos in topics:
                self.subs.append((t, qos))
                granted.append(min(qos, 1))     # QoS1까지 허용
                b.log("SUBSCRIBE", self.peer, "%r qos=%d" % (t, qos))
            self.send(M.suback(pid, granted))

        elif ptype == M.UNSUBSCRIBE:
            pid, topics = M.parse_unsubscribe(payload)
            self.subs = [s for s in self.subs if s[0] not in topics]
            self.send(M.unsuback(pid))

        elif ptype == M.PUBLISH:
            msg = M.parse_publish(flags, payload)
            b.log("PUBLISH↑", self.peer, "%r (%dB) qos=%d" % (
                msg["topic"], len(msg["payload"]), msg["qos"]))
            if msg["qos"] == 1:
                self.send(M.puback(msg["packet_id"]))
            if b.on_publish:
                b.on_publish(self, msg)         # 보일러가 올린 상태 → 우리 로직으로

        elif ptype == M.PINGREQ:
            self.send(M.pingresp())             # keepalive 유지

        elif ptype == M.DISCONNECT:
            self.alive = False
            b.log("DISCONNECT", self.peer, "")

        else:
            b.log("?", self.peer, "type=%s flags=%x len=%d" % (name, flags, len(payload)))


class Broker:
    """세션들을 묶는 허브. 콜백으로 바깥 로직(J07 변환)과 연결."""
    def __init__(self, logger=None, on_connect=None, on_publish=None):
        self.sessions = []
        self._log = logger or (lambda *a: None)
        self.on_connect = on_connect
        self.on_publish = on_publish
        self._pid = 0

    def log(self, tag, peer, msg):
        self._log("[%-11s] %s  %s" % (tag, peer, msg))

    def new_session(self, peer):
        s = Session(self, peer)
        self.sessions.append(s)
        return s

    def drop(self, s):
        if s in self.sessions:
            self.sessions.remove(s)

    def next_pid(self):
        self._pid = (self._pid % 65535) + 1
        return self._pid

    def publish_to(self, session, topic, payload, qos=1):
        """우리가 특정 보일러 세션에 명령(J07) 내려보내기."""
        pid = self.next_pid() if qos else 0
        session.send(M.publish(topic, payload, qos=qos, packet_id=pid))
        self.log("PUBLISH↓", session.peer, "%r (%dB)" % (topic, len(payload)))

    def publish_all(self, topic, payload, qos=1):
        """topic을 구독 중인 모든 세션에 내려보내기(와일드카드 미구현 — 완전일치/prefix)."""
        tb = topic.encode() if isinstance(topic, str) else topic
        for s in self.sessions:
            if any(_match(f, tb) for f, _ in s.subs):
                self.publish_to(s, topic, payload, qos)


def _match(filt, topic):
    """아주 단순한 토픽 매칭: 완전일치 또는 '#' 프리픽스."""
    if filt == topic:
        return True
    if filt.endswith(b"#"):
        return topic.startswith(filt[:-1])
    if filt.endswith(b"/+"):
        return topic.startswith(filt[:-1])
    return False
