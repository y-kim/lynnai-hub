#!/usr/bin/env python3
"""hub_bootstrap.py (대응 ESP32: hub_proxy.c 의 /key/server) -- Stand in for the Rinnai cloud to learn how our own boiler module talks to it, on our own LAN.
TLS ports log whether the module accepts our self-signed cert (cert-pinning test).
Plain ports (80 etc.) log the full request so we can see the API path and whether it is plain HTTP.

/key/server is now LIVE-RELAYED to the real cloud (by IP, not hostname -- our own DNS
override would otherwise resolve www.rkiot.net back to this Pi and loop) instead of
serving a cached bootstrap.xml. This lets us see the real, current uid/pass every time
-- e.g. across re-registering the device under a different account -- rather than a
stale snapshot from 2026-11-15."""
import os, socket, ssl, threading, datetime, http.client

# lynnai-nas 부트스트랩: 기기의 GET /key/server 를 진짜 린나이 클라우드(IP)로 다시 물어 그대로 돌려준다.
# (우리 DNS 가 www.rkiot.net 을 우리에게 돌리므로 반드시 IP 로 간다.) ESP32 hub_proxy_bootstrap_handler 의 파이썬 판.
# 원본: Pi 의 rkprobe/fakesrv.py — 경로를 환경변수로 바꾸고 TLS 포트(핀닝 테스트용)는 뺌.
TLS_PORTS = []
PLAIN_PORTS = [int(os.environ.get("HTTP_PORT", "80"))]
REAL_HOST_IP = os.environ.get("UPSTREAM_IP", "3.38.86.244")
REAL_SNI = os.environ.get("UPSTREAM_SNI", "www.rkiot.net")

def log(m):
    print(f"{datetime.datetime.now().isoformat(timespec='seconds')} [bootstrap] {m}", flush=True)

def sni_cb(sslobj, name, ctx):
    sslobj.server_hostname_seen = name

def handle_tls(raw, addr, port):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    try:
        ctx.minimum_version = ssl.TLSVersion.TLSv1
        ctx.set_ciphers("DEFAULT@SECLEVEL=0")   # allow the module's old TLS 1.1 / weak ciphers
    except Exception as e:
        pass
    ctx.load_cert_chain(CERT)
    ctx.sni_callback = sni_cb
    raw.settimeout(8)
    try:
        tls = ctx.wrap_socket(raw, server_side=True)
        sni = getattr(tls, "server_hostname_seen", None)
        log(f"[{port}] {addr[0]} TLS OK  SNI={sni}  -> ACCEPTED our cert (pinning NOT enforced)")
        try:
            tls.settimeout(6); d = tls.recv(2048)
            if d:
                try: log(f"[{port}] {addr[0]} req:\n{d.decode('utf-8','replace')}")
                except Exception: log(f"[{port}] {addr[0]} bytes: {d[:120].hex()}")
                tls.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}")
        except Exception: pass
        tls.close()
    except ssl.SSLError as e:
        log(f"[{port}] {addr[0]} TLS refused: {e.reason or e}  -> REJECTED our cert (pinning enforced)")
    except Exception as e:
        log(f"[{port}] {addr[0]} err: {e}")

def fetch_real_bootstrap(req_headers_txt):
    """Re-issue the module's /key/server GET against the real cloud (by IP), live, every call."""
    # pull any Ck-* headers the module sent so the real server sees the same request shape
    extra = {}
    for line in req_headers_txt.splitlines()[1:]:
        if ":" in line:
            k, _, v = line.partition(":")
            k = k.strip()
            if k.lower().startswith("ck-"):
                extra[k] = v.strip()
    extra["Host"] = REAL_SNI
    conn = http.client.HTTPConnection(REAL_HOST_IP, 80, timeout=8)
    try:
        conn.request("GET", "/key/server", headers=extra)
        r = conn.getresponse()
        body = r.read()
        return r.status, dict(r.getheaders()), body
    finally:
        conn.close()

def handle_plain(c, addr, port):
    c.settimeout(6)
    try:
        data = b""
        while len(data) < 4096:
            chunk = c.recv(1024)
            if not chunk: break
            data += chunk
            if b"\r\n\r\n" in data: break
        if data:
            try: txt = data.decode('utf-8','replace'); log(f"[{port}] {addr[0]} PLAIN req:\n{txt}")
            except Exception: txt=""; log(f"[{port}] {addr[0]} bytes: {data[:200].hex()}")
            if "/key/server" in txt:
                try:
                    status, hdrs, body = fetch_real_bootstrap(txt)
                    log(f"[{port}] {addr[0]} -> LIVE relay to real cloud, status={status}\n{body.decode('utf-8','replace')}")
                except Exception as e:
                    log(f"[{port}] {addr[0]} live relay failed: {e}")
                    c.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n"); return
                c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/xml; charset=utf-8\r\nContent-Length: "+str(len(body)).encode()+b"\r\n\r\n"+body)
            else:
                c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}")
        else:
            log(f"[{port}] {addr[0]} connected, no data")
    except Exception as e:
        log(f"[{port}] {addr[0]} plain err: {e}")
    finally:
        c.close()

def serve(port, tls):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try: s.bind(("0.0.0.0", port))
    except Exception as e: log(f"bind {port} failed: {e}"); return
    s.listen(16); log(f"listening on {port} ({'TLS' if tls else 'plain'})")
    while True:
        c, a = s.accept()
        log(f"[{port}] connection from {a[0]}:{a[1]}")
        tgt = handle_tls if tls else handle_plain
        threading.Thread(target=tgt, args=(c, a, port), daemon=True).start()

def main():
    for p in PLAIN_PORTS:
        threading.Thread(target=serve, args=(p, False), daemon=True).start()
    log("부트스트랩 시작; plain " + ",".join(map(str, PLAIN_PORTS)) + f" -> {REAL_HOST_IP}")
    threading.Event().wait()

if __name__ == "__main__":
    main()
