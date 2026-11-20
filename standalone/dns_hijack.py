"""
dns_hijack.py  --  최소 UDP DNS 응답기 (순수 로직 + 소켓, Pico 포팅 쉬움)

목적: 보일러가 묻는 rkiot.net(및 지정 도메인)을 우리 박스 IP로 답한다.
나머지 도메인은: forward=True면 업스트림 DNS로 넘기고(standalone 아님),
                 forward=False면 NXDOMAIN 또는 우리 IP(캡티브) 반환.

MicroPython 포팅: socket은 usocket, 나머지는 그대로(threading 이 없으면 포워딩은 그 자리에서).
포워딩은 질의마다 스레드(동시 FWD_MAX 개까지, 넘으면 버림 — 기기가 다시 묻는다)라 느린 상위 DNS 가 루프를 안 막는다.
"""

import socket
import struct
try:
    import threading
except ImportError:                 # MicroPython
    threading = None

BUF = 4096                          # EDNS 응답(512 넘음)을 자르지 않게. TC 비트는 그대로 넘겨 기기가 판단
FWD_MAX = 16


def _parse_qname(data, off):
    """DNS 이름 파싱(압축 미지원 질문부 기준). (name_str, next_off)."""
    labels = []
    while True:
        ln = data[off]; off += 1
        if ln == 0:
            break
        if ln & 0xC0:               # 압축 포인터 — 질문부엔 거의 없음
            off += 1
            break
        labels.append(data[off:off + ln]); off += ln
    return b".".join(labels).decode("ascii", "ignore").lower(), off


def _build_answer(query, ip):
    """질문 패킷 + A레코드 1개로 응답 조립. ip='a.b.c.d'."""
    tid = query[:2]
    # flags: QR=1, Opcode=0, AA=1, RD copy, RA=1, rcode=0
    rd = query[2] & 0x01
    flags = 0x80 | (0x04) | rd          # 0x8400 류 + RD
    hdr = tid + struct.pack(">BBHHHH", 0x85 if rd else 0x84, 0x00, 1, 1, 0, 0)
    # 질문부 그대로 복사 (헤더 12B 이후 ~ 0x00 + qtype2 + qclass2)
    off = 12
    while query[off] != 0:
        off += query[off] + 1
    qend = off + 1 + 4                   # null + qtype + qclass
    question = query[12:qend]
    # 답: name 포인터(0xC00C) + type A(1) + class IN(1) + TTL + rdlen4 + ip
    octs = bytes(int(x) for x in ip.split("."))
    answer = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + octs
    return hdr + question + answer


class DnsHijack:
    def __init__(self, listen_ip, answer_ip, hijack_names, logger=None,
                 forward=None, catch_all=False, only_src=None, is_src_ok=None, on_unregistered=None):
        """
        listen_ip   : 바인드할 IP (AP측 IP, 예 192.168.50.1)
        answer_ip   : 하이재킹 대상에 답할 IP (대개 listen_ip와 동일)
        hijack_names: ['rkiot.net', ...]  (그 이름 자체와 하위 이름만: www.rkiot.net 은 되고 rkiot.net.evil.com 은 안 됨)
        forward     : (ip, port) 업스트림 DNS. None이면 포워딩 안 함(standalone).
        catch_all   : True면 하이재킹 외 도메인도 answer_ip로 답(캡티브 포털식).
        only_src    : 이 출발지 IP 목록에서 온 질의만 하이재킹(=등록된 린나이 기기).
                      None/빈 목록이면 제한 없음. 목록 밖 기기의 질의는 forward 로 그대로 넘긴다.
        is_src_ok   : 함수(ip) -> bool. 주면 only_src 대신 이것으로 등록된 기기인지 그때그때 묻는다
                      (시작할 때 굳힌 목록이 아니라 살아 있는 등록부 — lynnai-nas). 예외가 나면 등록 안 된 것으로 본다.
        on_unregistered : 함수(ip, name). 등록 안 된 출발지가 하이재킹 대상 이름을 물을 때마다 부른다(후보 기록용).
                      True 를 돌려주면 그 자리에서 등록된 것으로 보고 이 질의부터 하이재킹한다. 아니면 평소처럼 넘긴다.
        """
        self.listen_ip = listen_ip
        self.answer_ip = answer_ip
        self.names = [n.lower() for n in hijack_names]
        self._log = logger or (lambda *a: None)
        self.forward = forward
        self.catch_all = catch_all
        self.only_src = set(only_src or [])
        self.is_src_ok = is_src_ok
        self.on_unregistered = on_unregistered
        self.sock = None
        self._slots = threading.BoundedSemaphore(FWD_MAX) if threading else None

    def _forward_now(self, data, addr, qname):
        up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            up.settimeout(3)
            up.connect(self.forward)          # 상위 DNS 가 아닌 곳에서 온 답은 받지 않게
            up.send(data)
            resp = up.recv(BUF)
            if len(resp) >= 2 and resp[:2] == data[:2]:   # 질의 id 가 같은 답만
                self.sock.sendto(resp, addr)
        except Exception as e:
            self._log("[dns] forward fail %s: %s" % (qname, e))
        finally:
            up.close()

    def _forward(self, data, addr, qname):
        if not self.forward:
            return                       # standalone(외부차단): 묵살
        if not self._slots:
            return self._forward_now(data, addr, qname)
        if not self._slots.acquire(False):
            self._log("[dns] forward 동시 %d개 꽉 참 — 버림 %s" % (FWD_MAX, qname))
            return

        def run():
            try:
                self._forward_now(data, addr, qname)
            finally:
                self._slots.release()
        try:
            threading.Thread(target=run, daemon=True).start()
        except Exception as e:
            self._slots.release()
            self._log("[dns] forward 스레드 실패: %s" % e)

    def open(self, port=53):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.listen_ip, port))
        self.sock = s
        self._log("[dns] listening %s:%d  hijack=%s -> %s" % (
            self.listen_ip, port, self.names, self.answer_ip))
        return s

    def _is_hijacked(self, qname):
        qname = qname.rstrip(".")
        return any(qname == n or qname.endswith("." + n) for n in self.names)

    def _src_ok(self, src, qname):
        """이 출발지의 질의를 하이재킹해도 되나(등록된 기기인가)."""
        if self.is_src_ok is None:
            ok = not self.only_src or src in self.only_src
        else:
            try:
                ok = bool(self.is_src_ok(src))
            except Exception as e:
                self._log("[dns] 등록 판정 오류 %s: %s" % (src, e))
                ok = False
        if not ok and self.on_unregistered and self._is_hijacked(qname):
            try:
                ok = self.on_unregistered(src, qname) is True
            except Exception as e:
                self._log("[dns] 후보 기록 오류 %s: %s" % (src, e))
        return ok

    def handle_once(self):
        """select 루프에서 readable일 때 1패킷 처리."""
        data, addr = self.sock.recvfrom(BUF)
        if len(data) < 12:
            return
        try:
            qname, _ = _parse_qname(data, 12)
        except Exception:
            return
        src = addr[0]
        # 등록된 린나이 기기 목록(또는 판정 함수)이 있으면, 등록 안 된 기기의 질의는 건드리지 않고 그대로 넘긴다.
        if not self._src_ok(src, qname):
            self._forward(data, addr, qname)
            return
        if self._is_hijacked(qname):
            self._log("[dns] HIJACK %s -> %s  (from %s)" % (qname, self.answer_ip, src))
            try:
                self.sock.sendto(_build_answer(data, self.answer_ip), addr)
            except (IndexError, ValueError, OSError):
                pass                     # 질문부가 깨진 패킷
        elif self.catch_all:
            try:
                self.sock.sendto(_build_answer(data, self.answer_ip), addr)
            except (IndexError, ValueError, OSError):
                pass
        else:
            self._forward(data, addr, qname)   # forward 없으면 묵살(standalone 외부차단)
