#!/usr/bin/env python3
"""hub_proxy.py (대응 ESP32: hub_proxy.c) -- lynnai-nas proxy 중계: 내 보일러 TLS 를 받아(우리 인증서) 진짜 린나이 클라우드로 다시 TLS 로 이어 준다.
양쪽을 그대로 포워딩하므로 린나이 앱은 계속 동작한다. ESP32 펌웨어 hub_proxy.c 의 파이썬 판.
(원본: Pi 의 rkprobe/bridge.py — 하드코딩 경로·기기 IP 를 환경변수로 바꾸고, 평문 payload 로깅은 기본 끔.)

환경변수: NAS_CERT, UPSTREAM_IP/UPSTREAM_PORT, UPSTREAM_SNI, RINNAI_IPS(중계할 기기, 쉼표),
          MQTT_PORT, DEBUG_MQTT(1이면 복호화 payload 까지 로그 — 비밀값 포함, 평소엔 끔)."""
import os, socket, ssl, threading

CERT = os.environ.get("NAS_CERT", "/data/certs/server.crt")
KEY = os.environ.get("NAS_KEY", "/data/certs/server.key")
LISTEN = ("0.0.0.0", int(os.environ.get("MQTT_PORT", "8883")))
UPSTREAM = (os.environ.get("UPSTREAM_IP", "3.38.86.244"), int(os.environ.get("UPSTREAM_PORT", "8883")))
UP_SNI = os.environ.get("UPSTREAM_SNI", "www.rkiot.net")
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
        up = socket.create_connection(UPSTREAM, timeout=10)
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
    cctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    cctx.check_hostname = False; cctx.verify_mode = ssl.CERT_NONE
    try: cctx.minimum_version = ssl.TLSVersion.TLSv1; cctx.set_ciphers("DEFAULT@SECLEVEL=0")
    except Exception: pass
    try:
        up = cctx.wrap_socket(socket.create_connection(UPSTREAM, timeout=10), server_hostname=UP_SNI)
    except Exception as e:
        log(f"{addr[0]} up TLS fail: {e}"); down.close(); return
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

def run(core=None):
    global CORE
    CORE = core
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(LISTEN); s.listen(16)
    log(f"중계 시작 :{LISTEN[1]} -> {UPSTREAM[0]}:{UPSTREAM[1]}  중계대상={DEVICE_IP or '모든 기기'}")
    while True:
        c, a = s.accept()
        threading.Thread(target=handle, args=(c, a), daemon=True).start()

if __name__ == "__main__":
    run()
