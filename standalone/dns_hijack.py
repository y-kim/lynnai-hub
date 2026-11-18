"""
dns_hijack.py  --  최소 UDP DNS 응답기 (순수 로직 + 소켓, Pico 포팅 쉬움)

목적: 보일러가 묻는 rkiot.net(및 지정 도메인)을 우리 박스 IP로 답한다.
나머지 도메인은: forward=True면 업스트림 DNS로 넘기고(standalone 아님),
                 forward=False면 NXDOMAIN 또는 우리 IP(캡티브) 반환.

MicroPython 포팅: socket은 usocket, 나머지는 그대로.
"""

import socket
import struct


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
                 forward=None, catch_all=False):
        """
        listen_ip   : 바인드할 IP (AP측 IP, 예 192.168.50.1)
        answer_ip   : 하이재킹 대상에 답할 IP (대개 listen_ip와 동일)
        hijack_names: ['rkiot.net', 'www.rkiot.net', ...]  (부분일치)
        forward     : (ip, port) 업스트림 DNS. None이면 포워딩 안 함(standalone).
        catch_all   : True면 하이재킹 외 도메인도 answer_ip로 답(캡티브 포털식).
        """
        self.listen_ip = listen_ip
        self.answer_ip = answer_ip
        self.names = [n.lower() for n in hijack_names]
        self._log = logger or (lambda *a: None)
        self.forward = forward
        self.catch_all = catch_all
        self.sock = None

    def open(self, port=53):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.listen_ip, port))
        self.sock = s
        self._log("[dns] listening %s:%d  hijack=%s -> %s" % (
            self.listen_ip, port, self.names, self.answer_ip))
        return s

    def _is_hijacked(self, qname):
        return any(n in qname for n in self.names)

    def handle_once(self):
        """select 루프에서 readable일 때 1패킷 처리."""
        data, addr = self.sock.recvfrom(512)
        if len(data) < 12:
            return
        try:
            qname, _ = _parse_qname(data, 12)
        except Exception:
            return
        if self._is_hijacked(qname):
            self._log("[dns] HIJACK %s -> %s  (from %s)" % (qname, self.answer_ip, addr[0]))
            self.sock.sendto(_build_answer(data, self.answer_ip), addr)
        elif self.catch_all:
            self.sock.sendto(_build_answer(data, self.answer_ip), addr)
        elif self.forward:
            try:
                up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                up.settimeout(3)
                up.sendto(data, self.forward)
                resp, _ = up.recvfrom(512)
                up.close()
                self.sock.sendto(resp, addr)
            except Exception as e:
                self._log("[dns] forward fail %s: %s" % (qname, e))
        # forward 없고 catch_all 아니면: 묵살 (standalone에서 외부차단)
