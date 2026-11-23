"""upstream.py (대응 ESP32: hub_proxy.c 의 resolve_up·up_connect) -- 진짜 린나이 클라우드 주소.

UPSTREAM_IP 를 주면 그 값만 쓴다. 비우면(기본) www.rkiot.net 을 이 컨테이너의 DNS(/etc/resolv.conf, 보통 공유기)에
직접 물어 **답에 든 주소들을 그 TTL 동안만** 쓴다(30초~1시간 사이로 다듬는다). TTL 이 지나면 다시 묻고, 그때 DNS 가
답하지 않으면 주소가 없는 것이다 — 예전 주소나 내장 기본 주소로 버티지 않는다(자동 단독 동작의 까닭 "주소 조회 실패").
주소가 여럿이면 차례로 시도하고, 안 되는 주소는 뒤로 돌린다.

NAS 의 DNS 하이재커는 등록된 린나이 기기에게만 답을 바꾸므로 NAS 자신은 진짜 주소를 받는다. 그래도 답이 공인
유니캐스트가 아니면(사설·루프백·링크로컬·CGNAT·멀티캐스트·예약 — 우리 하이재커가 답한 경우 등) 그 주소는 버린다.
"""
import ipaddress
import os
import socket
import struct
import threading
import time

HOST = os.environ.get("UPSTREAM_SNI", "www.rkiot.net")
FIXED = os.environ.get("UPSTREAM_IP", "").strip()
TTL_MIN, TTL_MAX = 30, 3600
MAX_IPS = 4
_ips = []                      # DNS 가 준 주소들(실패한 것은 뒤로)
_exp = 0.0                     # 이 주소들을 쓸 수 있는 마지막 시각(time.monotonic)
_lock = threading.Lock()


class NoAddress(OSError):
    """서버 주소를 알아내지 못함(DNS 가 답하지 않음)."""


def _usable(a):
    """클라우드 주소로 쓸 수 있는 공인 유니캐스트인가(ipaddress 판정; 100.64.0.0/10 은 is_global 이 거름)."""
    return (a.version == 4 and a.is_global and not a.is_multicast and not a.is_reserved
            and not a.is_unspecified and not a.is_loopback and not a.is_link_local and not a.is_private)


def _resolvers():
    out = []
    try:
        with open("/etc/resolv.conf") as f:
            for line in f:
                p = line.split()
                if len(p) >= 2 and p[0] == "nameserver" and ":" not in p[1]:
                    out.append(p[1])
    except OSError:
        pass
    return out[:3]


def _query(server, name, timeout=2.0):
    """server 에 name 의 A 레코드를 묻는다 → ([주소…], 가장 짧은 TTL). 답이 없거나 오류면 ([], 0)."""
    qid = os.urandom(2)
    q = (qid + b"\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00"
         + b"".join(bytes([len(p)]) + p.encode() for p in name.split(".")) + b"\x00\x00\x01\x00\x01")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(q, (server, 53))
        r, _ = s.recvfrom(1024)
    except OSError:
        return [], 0
    finally:
        s.close()
    if len(r) < 12 or r[:2] != qid or r[3] & 0x0F:
        return [], 0
    pos, ips, ttl = len(q), [], None
    try:
        for _ in range(struct.unpack(">H", r[6:8])[0]):
            if r[pos] & 0xC0 == 0xC0:
                pos += 2
            else:
                while r[pos]:
                    pos += r[pos] + 1
                pos += 1
            typ, _cls, t, ln = struct.unpack(">HHIH", r[pos:pos + 10])
            pos += 10
            if typ == 1 and ln == 4:
                ips.append(".".join(str(b) for b in r[pos:pos + 4]))
                ttl = t if ttl is None else min(ttl, t)
            pos += ln
    except (IndexError, struct.error):
        return [], 0
    return ips[:MAX_IPS], ttl or 0


def ips(log=None):
    """지금 쓸 서버 주소들. TTL 이 남았으면 받아 둔 것, 지났으면 다시 물은 것, 답이 없으면 []."""
    global _ips, _exp
    if FIXED:
        return [FIXED]
    with _lock:
        if _ips and time.monotonic() < _exp:
            return list(_ips)
        got, ttl, who = [], 0, ""
        for srv in _resolvers():
            got, ttl = _query(srv, HOST)
            got = [a for a in got if _usable(ipaddress.ip_address(a))]
            if got:
                who = srv
                break
        if got:
            use = min(max(ttl, TTL_MIN), TTL_MAX)
            if log and got != _ips:
                log(f"[upstream] {HOST} = {', '.join(got)} (TTL {ttl}초 → {use}초 동안 씀, DNS {who})")
            _ips, _exp = got, time.monotonic() + use
        else:
            if log and _ips:
                log(f"[upstream] {HOST} 조회 실패 — 받아 둔 주소의 TTL 이 지나 주소를 비움")
            _ips, _exp = [], 0.0
        return list(_ips)


def ip(log=None):
    """첫 주소(없으면 NoAddress). 표시용·옛 호출부용 — 연결은 connect() 를 쓴다."""
    a = ips(log)
    if not a:
        raise NoAddress(f"{HOST} 주소를 알아내지 못함")
    return a[0]


def connect(port, timeout=10, log=None):
    """서버에 TCP 로 붙는다: 주소를 앞에서부터 차례로, 실패한 주소는 맨 뒤로. 주소가 없으면 NoAddress, 다 실패하면 OSError."""
    global _ips
    addrs = ips(log)
    if not addrs:
        raise NoAddress(f"{HOST} 주소를 알아내지 못함")
    err = None
    for a in addrs:
        try:
            return socket.create_connection((a, port), timeout=min(timeout, 5) if len(addrs) > 1 else timeout)
        except OSError as e:
            err = e
            if log:
                log(f"[upstream] {a}:{port} 연결 안 됨({e})")
            with _lock:
                if len(_ips) > 1 and _ips[0] == a:
                    _ips = _ips[1:] + [a]
    raise err
