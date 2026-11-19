#!/usr/bin/env python3
"""hub_proxy.py (대응 ESP32: hub_proxy.c) -- lynnai-nas 의 기기 연결(:8883). 내 보일러 TLS 를 받아(우리 인증서)
  proxy(기본)   : 진짜 린나이 클라우드로 다시 TLS 로 이어 준다. 양쪽을 그대로 포워딩하므로 린나이 앱은 계속 동작.
  standalone    : 클라우드에 안 붙고 허브가 MQTT 상대(CONNACK·SUBACK·PUBACK·PINGRESP)가 된다. 상태는 코어로,
                  상태 요청·시간 응답·SmartThings 명령은 코어가 넣는다(MODE=standalone).
ESP32 펌웨어 hub_proxy.c 의 파이썬 판.
(원본: Pi 의 rkprobe/bridge.py — 하드코딩 경로·기기 IP 를 환경변수로 바꾸고, 평문 payload 로깅은 기본 끔.)

환경변수: NAS_CERT, UPSTREAM_IP/UPSTREAM_PORT, UPSTREAM_SNI, RINNAI_IPS(중계할 기기, 쉼표),
          UPSTREAM_CA(클라우드 사설 CA, 기본 이 폴더의 cloud_ca.crt — 비우면 검증 안 함),
          MQTT_PORT, DEBUG_MQTT(1이면 복호화 payload 까지 로그 — 비밀값 포함, 평소엔 끔)."""
import os, socket, ssl, threading, time
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
UP_CN = os.environ.get("UPSTREAM_CN", "dev2.ckpush.com")   # 클라우드 인증서의 실제 이름(SAN 없이 CN 만) — 체인에 더해 이름도 고정
DEBUG_MQTT = os.environ.get("DEBUG_MQTT", "") not in ("", "0")   # payload 에 비밀값이 있어 기본 끔
TYPES = {1:"CONNECT",2:"CONNACK",3:"PUBLISH",4:"PUBACK",8:"SUBSCRIBE",9:"SUBACK",
         10:"UNSUBSCRIBE",12:"PINGREQ",13:"PINGRESP",14:"DISCONNECT"}

def log(m, f=None):
    import datetime
    print(f"{datetime.datetime.now().isoformat(timespec='seconds')} [relay] {m}", flush=True)

def parse(buf, arrow, on_publish=None):
    """Pull whole MQTT packets out of buf; return leftover. Log PUBLISH/SUBSCRIBE.
    on_publish(topic, payload_bytes) 가 있으면 PUBLISH 마다 부른다(상태 스눕용)."""
    i = 0
    while i < len(buf):
        if len(buf) - i < 2: break
        b0 = buf[i]; ptype = b0 >> 4
        # remaining length varint
        mult = 1; val = 0; j = i + 1; ok = False
        while j < len(buf):
            enc = buf[j]; val += (enc & 0x7f) * mult; j += 1
            if enc & 0x80 == 0: ok = True; break
            mult *= 128
            if mult > 128**3: ok = True; break
        if not ok: break
        if len(buf) - j < val: break           # packet not fully arrived
        body = buf[j:j+val]; i = j + val
        name = TYPES.get(ptype, f"?{ptype}")
        if ptype == 3:  # PUBLISH
            tl = (body[0] << 8) | body[1]; topic = body[2:2+tl].decode("utf-8","replace")
            rest = body[2+tl:]
            if (b0 >> 1) & 3 > 0: rest = rest[2:]   # strip packet id if QoS>0
            if DEBUG_MQTT:
                try: payload = rest.decode("utf-8","replace")
                except Exception: payload = rest.hex()
                log(f"{arrow} PUBLISH {topic}  {payload}")   # 비밀값 포함 — DEBUG_MQTT 일 때만
            else:
                log(f"{arrow} PUBLISH {topic}  ({len(rest)}B)")
            if on_publish:
                try: on_publish(topic, rest)
                except Exception as e: log(f"{arrow} on_publish 오류: {e}")
        elif ptype == 8:  # SUBSCRIBE
            rest = body[2:]; topics = []
            while len(rest) >= 2:
                tl = (rest[0] << 8) | rest[1]; topics.append(rest[2:2+tl].decode("utf-8","replace")); rest = rest[2+tl+1:]
            log(f"{arrow} SUBSCRIBE {topics}")
        elif ptype == 1:  # CONNECT -- variable header: proto name(2+len) + level(1) + flags(1) + keepalive(2), then payload: clientId(2+len)
            try:
                pl = (body[0] << 8) | body[1]
                off = 2 + pl + 1 + 1 + 2   # skip proto name, level, flags, keepalive
                cl = (body[off] << 8) | body[off+1]
                client_id = body[off+2:off+2+cl].decode("utf-8", "replace")
                log(f"{arrow} CONNECT clientId={client_id!r}")
            except Exception as e:
                log(f"{arrow} CONNECT (clientId parse failed: {e})")
        elif ptype == 14:
            log(f"{arrow} {name}")
    return buf[i:]

def pump(src, dst, arrow, decode=True, on_publish=None, drain=None):
    """src → dst 로 포워딩. decode 면 MQTT 로 로그/스눕. drain(dst) 는 패킷 경계마다 불러
    기기 쪽에 명령을 끼워 넣는다(cloud→device 방향에서만. 흐름 한가운데 끼면 깨진다)."""
    buf = b""; why = "?"
    try:
        while True:
            d = src.recv(4096)
            if not d: why = "EOF(peer FIN)"; break
            dst.sendall(d)
            if decode:
                buf += d
                try: buf = parse(buf, arrow, on_publish)
                except Exception: buf = b""
                if drain and not buf:          # 패킷 경계: 지금 끼워 넣어도 안전
                    try: drain(dst)
                    except Exception as e: log(f"{arrow} drain 오류: {e}")
    except Exception as e:
        why = f"{type(e).__name__}:{e}"
    finally:
        log(f"{arrow} pump end: {why}")
        try: dst.shutdown(socket.SHUT_WR)
        except Exception: pass

# 중계(TLS 종단)할 기기 = 등록된 린나이 기기. 비우면 모두 중계. 그 밖의 클라이언트는 passthrough.
DEVICE_IP = {ip.strip() for ip in os.environ.get("RINNAI_IPS", "").replace(";", ",").split(",") if ip.strip()}

def passthrough(raw, addr):
    """Non-device client (e.g. the NAS logger): forward its TLS straight to the real cloud so its own
    cert check succeeds. We never terminate TLS here, so we do not break pinned clients."""
    try:
        up = socket.create_connection((upstream.ip(log), UP_PORT), timeout=10)
    except Exception as e:
        log(f"{addr[0]} passthrough upstream fail: {e}"); 
        try: raw.close()
        except Exception: pass
        return
    t1 = threading.Thread(target=pump, args=(raw, up, "P>"), kwargs={"decode": False}, daemon=True)
    t2 = threading.Thread(target=pump, args=(up, raw, "P<"), kwargs={"decode": False}, daemon=True)
    t1.start(); t2.start(); t1.join(); t2.join()
    for sck in (raw, up):
        try: sck.close()
        except Exception: pass

CORE = None   # 설정되면 상태 스눕 + 명령 주입을 한다(hub_core.HubCore). 없으면 순수 중계만.

def handle(raw, addr):
    # 등록된 린나이 기기만 TLS 종단(중계), 나머지(로거·헬스체크 등)는 passthrough 로 안 깨뜨린다.
    if DEVICE_IP and addr[0] not in DEVICE_IP:
        passthrough(raw, addr); return
    sctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    try:
        sctx.minimum_version = ssl.TLSVersion.TLSv1; sctx.set_ciphers("DEFAULT@SECLEVEL=0")
    except Exception: pass
    sctx.load_cert_chain(CERT, KEY)
    raw.settimeout(30)
    try: down = sctx.wrap_socket(raw, server_side=True)
    except Exception as e: log(f"{addr[0]} down TLS fail: {e}"); return
    if STANDALONE:
        return broker(down, addr)
    if not UP_CA or not os.path.exists(UP_CA):
        log(f"{addr[0]} 클라우드 CA 없음 — 검증 없이는 붙지 않음"); down.close(); return
    cctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    cctx.check_hostname = False                    # 접속 이름(www.rkiot.net)과 인증서 이름이 달라 아래에서 직접 비교
    cctx.verify_mode = ssl.CERT_REQUIRED; cctx.load_verify_locations(UP_CA)   # 가짜 클라우드 차단
    try: cctx.minimum_version = ssl.TLSVersion.TLSv1; cctx.set_ciphers("DEFAULT@SECLEVEL=0")
    except Exception: pass
    try:
        up = cctx.wrap_socket(socket.create_connection((upstream.ip(log), UP_PORT), timeout=10), server_hostname=UP_SNI)
    except Exception as e:
        log(f"{addr[0]} up TLS fail: {e}"); down.close(); return
    cn = dict(x[0] for x in (up.getpeercert() or {}).get("subject", ())).get("commonName")
    if cn != UP_CN:
        log(f"{addr[0]} 클라우드 인증서 이름 다름({cn!r} ≠ {UP_CN!r}) — 연결 안 함"); up.close(); down.close(); return
    down.settimeout(None); up.settimeout(None)
    log(f"{addr[0]} bridged  module({down.version()}) <-> cloud({up.version()})")

    # 상태 스눕 + 명령 주입(core 가 있을 때). 명령은 C->M(클라우드→기기) 경계에서만 down 에 쓴다.
    link = None; pending = []; plock = threading.Lock()
    on_pub = drain = None
    if CORE is not None:
        def inject(pkt):
            with plock: pending.append(pkt)
        link = CORE.link_open(inject)
        def on_pub(topic, payload): CORE.on_device_publish(link, topic, payload)
        def drain(dst):
            with plock:
                q, pending[:] = pending[:], []
            for pkt in q:
                dst.sendall(pkt)
                log(f"{addr[0]} 명령 주입 {len(pkt)}B")

    t1 = threading.Thread(target=pump, args=(down, up, "M->C"), kwargs={"on_publish": on_pub}, daemon=True)
    t2 = threading.Thread(target=pump, args=(up, down, "C->M"), kwargs={"drain": drain}, daemon=True)
    t1.start(); t2.start(); t1.join(); t2.join()
    if link is not None:
        CORE.link_close(link)
    for s in (down, up):
        try: s.close()
        except Exception: pass
    log(f"{addr[0]} closed")

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
            with plock: pending.append(pkt)
        link = CORE.link_open(inject)
    log(f"{addr[0]} 연결됨: 기기({down.version()}) <-> 허브(단독 모드)")
    down.settimeout(0.3)
    buf = b""; last = time.time(); why = "?"
    try:
        while True:
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
            while len(buf) >= 2:                 # 다 모인 패킷마다
                mult, val, j, ok = 1, 0, 1, False
                while j < len(buf) and j <= 4:
                    enc = buf[j]; val += (enc & 0x7f) * mult; j += 1
                    if not enc & 0x80: ok = True; break
                    mult *= 128
                if not ok:
                    if j > 4: raise ValueError("길이 필드 깨짐")
                    break
                if len(buf) - j < val: break
                b0, body, buf = buf[0], buf[j:j + val], buf[j + val:]
                t = b0 >> 4
                if t == 3 and CORE is not None:
                    tl = (body[0] << 8) | body[1]
                    topic = body[2:2 + tl].decode("utf-8", "replace")
                    rest = body[2 + tl + (2 if (b0 >> 1) & 3 else 0):]
                    log(f"{addr[0]} 기기→허브 PUBLISH {topic.rsplit('/', 1)[-1]} {len(rest)}B")
                    CORE.on_device_publish(link, topic, rest)
                r = _reply(b0, body)
                if r is None: why = "DISCONNECT"; raise StopIteration
                if r: down.sendall(r)
                if t == 1: log(f"{addr[0]} CONNECT → CONNACK")
                if t == 8:
                    for tp in _topics(t, body):
                        log(f"{addr[0]} SUBSCRIBE {tp} → SUBACK")
                        if CORE is not None: CORE.on_device_subscribe(link, tp)
    except StopIteration:
        pass
    except Exception as e:
        why = f"{type(e).__name__}:{e}"
    finally:
        if link is not None: CORE.link_close(link)
        try: down.close()
        except Exception: pass
        log(f"{addr[0]} 연결 종료 ({why})")

def run(core=None):
    global CORE
    CORE = core
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(LISTEN); s.listen(16)
    if not STANDALONE and not UP_CA:
        log("오류: UPSTREAM_CA 가 비어 있음 — 검증 없이는 클라우드에 붙지 않음")
    elif not STANDALONE and not os.path.exists(UP_CA):
        log(f"오류: UPSTREAM_CA 파일 없음({UP_CA}) — 클라우드에 붙지 않음")
    log(f"기기 대기 :{LISTEN[1]}  {'단독 모드(클라우드 없음)' if STANDALONE else '프록시 -> ' + upstream.ip(log) + ':' + str(UP_PORT)}"
        f"  대상={DEVICE_IP or '모든 기기'}")
    while True:
        c, a = s.accept()
        threading.Thread(target=handle, args=(c, a), daemon=True).start()

if __name__ == "__main__":
    run()
