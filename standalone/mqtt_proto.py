"""
mqtt_proto.py  --  MQTT 3.1.1 패킷 인/디코드 (순수 로직, Pico 포팅 대상)

의존성: 없음 (struct만). CPython / MicroPython 공용.
transport(소켓/TLS)는 몰라야 한다. 바이트만 다룬다.

패킷 고정헤더: [type|flags (1B)] [remaining length (varint 1~4B)] [variable header + payload]
"""

# --- 패킷 타입 ---
CONNECT     = 1
CONNACK     = 2
PUBLISH     = 3
PUBACK      = 4
SUBSCRIBE   = 8
SUBACK      = 9
UNSUBSCRIBE = 10
UNSUBACK    = 11
PINGREQ     = 12
PINGRESP    = 13
DISCONNECT  = 14

NAMES = {
    1: "CONNECT", 2: "CONNACK", 3: "PUBLISH", 4: "PUBACK",
    5: "PUBREC", 6: "PUBREL", 7: "PUBCOMP", 8: "SUBSCRIBE", 9: "SUBACK",
    10: "UNSUBSCRIBE", 11: "UNSUBACK", 12: "PINGREQ", 13: "PINGRESP",
    14: "DISCONNECT",
}


# --- remaining length (가변 길이 정수) ---
def encode_len(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            break
    return bytes(out)


def decode_len(buf, i):
    """buf[i]부터 remaining length 읽기. (값, 다음인덱스) 반환. 부족하면 (None, i)."""
    mult = 1
    val = 0
    while True:
        if i >= len(buf):
            return None, i          # 아직 바이트 더 필요
        b = buf[i]
        i += 1
        val += (b & 0x7F) * mult
        if not (b & 0x80):
            return val, i
        mult *= 128
        if mult > 128 * 128 * 128:
            raise ValueError("remaining length too long")


def _u16(buf, i):
    return (buf[i] << 8) | buf[i + 1], i + 2


def _str(buf, i):
    n, i = _u16(buf, i)
    return buf[i:i + n], i + n      # bytes 반환 (디코딩은 호출측)


# --- 입력 스트림에서 패킷 하나 떼기 ---
def take_packet(buf):
    """
    bytearray(buf)에서 완성된 패킷 1개를 떼서 반환.
    반환: (ptype, flags, payload_bytes)  또는  None(아직 부족).
    소비한 만큼 buf를 앞에서 잘라낸다 (del buf[:n]).
    """
    if len(buf) < 2:
        return None
    b0 = buf[0]
    ptype = (b0 >> 4) & 0x0F
    flags = b0 & 0x0F
    rl, j = decode_len(buf, 1)
    if rl is None:
        return None
    if len(buf) < j + rl:
        return None                 # 본문 아직 덜 옴
    payload = bytes(buf[j:j + rl])
    del buf[:j + rl]
    return ptype, flags, payload


# --- 서버→클라 응답 빌더 ---
def connack(session_present=0, code=0):
    # 0x20, len=2, [ack flags][return code]  code 0 = accepted
    return bytes([0x20, 0x02, session_present & 1, code])


def suback(packet_id, granted_qos):
    body = bytearray([(packet_id >> 8) & 0xFF, packet_id & 0xFF])
    body.extend(granted_qos)        # 각 토픽당 1바이트 (0/1/2 또는 0x80 실패)
    return bytes([0x90]) + encode_len(len(body)) + bytes(body)


def unsuback(packet_id):
    return bytes([0xB0, 0x02, (packet_id >> 8) & 0xFF, packet_id & 0xFF])


def puback(packet_id):
    return bytes([0x40, 0x02, (packet_id >> 8) & 0xFF, packet_id & 0xFF])


def pingresp():
    return bytes([0xD0, 0x00])


def publish(topic, payload, qos=0, packet_id=0, retain=0, dup=0):
    """서버가 구독자에게 내려보내는 PUBLISH."""
    if isinstance(topic, str):
        topic = topic.encode()
    if isinstance(payload, str):
        payload = payload.encode()
    vh = bytearray([(len(topic) >> 8) & 0xFF, len(topic) & 0xFF])
    vh.extend(topic)
    if qos > 0:
        vh.extend([(packet_id >> 8) & 0xFF, packet_id & 0xFF])
    vh.extend(payload)
    b0 = 0x30 | (dup << 3) | (qos << 1) | (retain & 1)
    return bytes([b0]) + encode_len(len(vh)) + bytes(vh)


# --- 클라→서버 파서 ---
def parse_connect(payload):
    """CONNECT variable header+payload → dict(client_id, keepalive, user, ...)."""
    i = 0
    proto_name, i = _str(payload, i)        # "MQTT" or "MQIsdp"
    level = payload[i]; i += 1
    flags = payload[i]; i += 1
    keepalive, i = _u16(payload, i)
    client_id, i = _str(payload, i)
    out = {
        "proto": bytes(proto_name), "level": level, "flags": flags,
        "keepalive": keepalive, "client_id": bytes(client_id),
        "will_topic": None, "will_payload": None, "user": None, "password": None,
    }
    if flags & 0x04:                        # will
        wt, i = _str(payload, i)
        wp, i = _str(payload, i)
        out["will_topic"], out["will_payload"] = bytes(wt), bytes(wp)
    if flags & 0x80:                        # username
        u, i = _str(payload, i)
        out["user"] = bytes(u)
    if flags & 0x40:                        # password
        p, i = _str(payload, i)
        out["password"] = bytes(p)
    return out


def parse_publish(flags, payload):
    """PUBLISH → dict(topic, payload, qos, packet_id, retain)."""
    qos = (flags >> 1) & 0x03
    i = 0
    topic, i = _str(payload, i)
    pid = 0
    if qos > 0:
        pid, i = _u16(payload, i)
    return {
        "topic": bytes(topic), "payload": payload[i:],
        "qos": qos, "packet_id": pid, "retain": flags & 1,
    }


def parse_subscribe(payload):
    """SUBSCRIBE → (packet_id, [(topic_bytes, qos), ...])."""
    i = 0
    pid, i = _u16(payload, i)
    topics = []
    while i < len(payload):
        t, i = _str(payload, i)
        qos = payload[i]; i += 1
        topics.append((bytes(t), qos))
    return pid, topics


def parse_unsubscribe(payload):
    i = 0
    pid, i = _u16(payload, i)
    topics = []
    while i < len(payload):
        t, i = _str(payload, i)
        topics.append(bytes(t))
    return pid, topics
