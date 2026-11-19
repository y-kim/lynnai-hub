"""upstream.py (대응 ESP32: hub_proxy.c 의 resolve_up) -- 진짜 린나이 클라우드 주소.

UPSTREAM_IP 를 주면 그 값. 비우면(기본) www.rkiot.net 을 이 컨테이너의 시스템 DNS(보통 공유기)로 조회해 쓴다
(10분 재사용). NAS 의 DNS 하이재커는 등록된 린나이 기기에게만 답을 바꾸므로 NAS 자신은 진짜 주소를 받는다.
조회 결과가 공인 유니캐스트가 아니면(사설·루프백·링크로컬·CGNAT·멀티캐스트·예약 — 우리 하이재커가 답한 경우 등)
버리고 마지막으로 알던 주소를 쓴다.
"""
import ipaddress
import os
import socket
import threading
import time

HOST = os.environ.get("UPSTREAM_SNI", "www.rkiot.net")
FIXED = os.environ.get("UPSTREAM_IP", "").strip()
_last = "3.38.86.244"          # 처음 쓰는 값(조회 전·실패 시)
_at = 0.0
_lock = threading.Lock()


def _usable(a):
    """클라우드 주소로 쓸 수 있는 공인 유니캐스트인가(ipaddress 판정; 100.64.0.0/10 은 is_global 이 거름)."""
    return (a.version == 4 and a.is_global and not a.is_multicast and not a.is_reserved
            and not a.is_unspecified and not a.is_loopback and not a.is_link_local and not a.is_private)


def ip(log=None):
    global _last, _at
    if FIXED:
        return FIXED
    with _lock:
        if time.time() - _at < 600:
            return _last
        try:
            got = socket.gethostbyname(HOST)
            a = ipaddress.ip_address(got)
            if not _usable(a):
                raise ValueError(f"공인 주소 아님 {got}")
            if got != _last and log:
                log(f"[upstream] {HOST} 주소 {_last} → {got}")
            _last, _at = got, time.time()
        except Exception as e:
            if log:
                log(f"[upstream] {HOST} 조회 실패({e}) — {_last} 사용")
        return _last
