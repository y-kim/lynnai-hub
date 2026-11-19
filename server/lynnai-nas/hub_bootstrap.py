#!/usr/bin/env python3
"""hub_bootstrap.py (대응 ESP32: hub_proxy.c 의 boot_fetch) -- 기기의 GET /key/server 를 진짜 린나이 클라우드(IP)로
다시 물어 (상태, 헤더, 본문)을 돌려준다. 받는 쪽·저장·검증은 hub_api.py.
(우리 DNS 가 www.rkiot.net 을 우리에게 돌리므로 반드시 IP 로 간다.)
원본: Pi 의 rkprobe/fakesrv.py — 핀닝 시험용 TLS 포트·자체 HTTP 서버(계정 본문을 로그하던 것)는 뺐다.
"""
import datetime
import http.client
import os

import upstream                      # 진짜 클라우드 주소(UPSTREAM_IP 또는 DNS 조회)

REAL_SNI = os.environ.get("UPSTREAM_SNI", "www.rkiot.net")
MAX_BODY = 16 * 1024                 # 정상 응답은 수백 바이트. 넘으면 버림


def log(m):
    print(f"{datetime.datetime.now().isoformat(timespec='seconds')} [bootstrap] {m}", flush=True)


def fetch_real_bootstrap(req_headers_txt):
    """기기 요청의 Ck-* 헤더만 실어 진짜 클라우드에 GET /key/server. 본문이 MAX_BODY 를 넘으면 ValueError."""
    extra = {}
    for line in req_headers_txt.splitlines()[1:]:
        if ":" in line:
            k, _, v = line.partition(":")
            k = k.strip()
            if k.lower().startswith("ck-"):
                extra[k] = v.strip()
    extra["Host"] = REAL_SNI
    conn = http.client.HTTPConnection(upstream.ip(log), 80, timeout=8)
    try:
        conn.request("GET", "/key/server", headers=extra)
        r = conn.getresponse()
        body = r.read(MAX_BODY + 1)
        if len(body) > MAX_BODY:
            raise ValueError(f"응답이 너무 큼(>{MAX_BODY}B)")
        return r.status, dict(r.getheaders()), body
    finally:
        conn.close()
