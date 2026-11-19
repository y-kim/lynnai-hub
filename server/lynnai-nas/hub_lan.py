"""
hub_lan.py (대응 ESP32: hub_lan.c) -- SmartThings LAN Edge 드라이버가 붙는 통로. ESP32 펌웨어 hub_lan.c 의 파이썬 판.
드라이버(edge-driver-lan)는 Zigbee 든 LAN 이든 같은 core.lua 를 쓰므로, 이 통로만 열면 그대로 붙는다.

찾기(SSDP): 239.255.255.250:1900 의 M-SEARCH(ST: urn:lynnai-com:device:hub:1 / ssdp:all)에
  HTTP/1.1 200 OK + LYNNAI-ID + LYNNAI-PORT 로 답한다.
연결(TCP 8484, 줄 단위):
  드라이버 → "hello <토큰32hex>"   허브 → "ok <id>" | "denied"(1초 늦게) | "busy"
  이후 양쪽 "<명령 16진 2자리> <내용 16진>\n",  허브는 30초마다 "ping"
토큰: 새 드라이버는 페어링 창(pairing_open, NAS 는 LAN_PAIR=1 로 시작하면 처음 600초)에서만 등록(첫 설치도 같음).
  최대 4개, 꽉 차면 가장 오래 전에 등록한 것부터 바꾼다. 토큰 파일을 못 읽으면(없는 것 말고) 등록도 안 받는다.
인증 전 연결은 동시에 MAX_PREAUTH 개, 5초 안에 hello 없으면 끊음. 보내기는 기다리지 않는다(읽지 않는 상대는 끊음).
"""
import hmac
import json
import os
import select
import socket
import struct
import threading
import time

SSDP_ST = "urn:lynnai-com:device:hub:1"
MAX_CLIENTS = 2
MAX_TOKENS = 4
MAX_PREAUTH = 2
HELLO_SEC = 5
DENY_DELAY = 1.0
MAX_LINE = 512                  # 인증 전엔 넘으면 끊고, 뒤엔 버림
_HEX = set("0123456789abcdef")


def _token_ok(t):
    return isinstance(t, str) and len(t) == 32 and all(c in _HEX for c in t)


class LanUplink:
    def __init__(self, core, hub_id, nas_ip, data_dir, port=8484, log=print):
        self.core = core
        self.id = hub_id
        self.nas_ip = nas_ip
        self.port = port
        self.log = log
        self.tok_path = os.path.join(data_dir, "lan_tokens.json")
        self.tok_bad = False        # 파일이 있는데 못 읽음 → 등록 막음(덮어써서 기존 토큰을 잃지 않게)
        self.tokens = self._load_tokens()
        self.pair_until = 0
        self.clients = []           # 인증된 소켓
        self.preauth = 0
        self.lock = threading.Lock()

    # ---- 토큰 ----
    def _load_tokens(self):
        try:
            with open(self.tok_path) as f:
                v = json.load(f)
            if not isinstance(v, list) or not all(_token_ok(t) for t in v):
                raise ValueError("형식이 다름")
            self.tok_bad = False
            return list(dict.fromkeys(v))[-MAX_TOKENS:]
        except FileNotFoundError:
            self.tok_bad = False
            return []
        except Exception as e:
            if not self.tok_bad:
                self.log(f"[lan] 토큰 파일을 못 읽음({e}) — 고치거나 지울 때까지 모든 드라이버 거절")
            self.tok_bad = True
            return []

    def _save_tokens(self, toks):
        tmp = self.tok_path + ".tmp"
        try:
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                json.dump(toks, f)
            os.replace(tmp, self.tok_path)
            return True
        except Exception as e:
            self.log(f"[lan] 토큰 저장 실패: {e}")
            return False

    def pairing_open(self, seconds=180):
        self.pair_until = time.monotonic() + seconds
        self.log(f"[lan] 페어링 {seconds}초 열림")

    def _accept_token(self, tok):
        if not _token_ok(tok):
            return False
        with self.lock:
            if self.tok_bad:
                self.tokens = self._load_tokens()     # 파일을 고쳤거나 지웠으면 다시
                if self.tok_bad:
                    return False
            known = False
            for t in self.tokens:                     # 하나 맞아도 끝까지(시간 차이 줄이기)
                known |= hmac.compare_digest(t, tok)
            if known:
                return True
            if time.monotonic() >= self.pair_until:
                return False
            toks = (self.tokens + [tok])[-MAX_TOKENS:]
            if not self._save_tokens(toks):            # 저장 못 하면 등록 안 함(재시작 뒤 사라지지 않게)
                return False
            self.tokens = toks
            self.log("[lan] 새 드라이버 등록(페어링)")
            return True

    # ---- 내보내기(core → 모든 드라이버) ----
    @staticmethod
    def _send_now(conn, b):
        """기다리지 않고 보낸다. 다 못 보내면(상대가 안 읽어 버퍼 참) 끊는다 — 받는 스레드가 정리."""
        try:   # 제한시간 있는 소켓은 버퍼가 꽉 차면 send 가 기다리므로, 쓸 자리가 있는지 먼저 본다
            if select.select([], [conn], [], 0)[1] and conn.send(b, socket.MSG_DONTWAIT) == len(b):
                return True
        except (OSError, ValueError):
            pass
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        return False

    def send(self, cmd_id, data):
        b = (("%02x " % cmd_id) + data.hex() + "\n").encode()
        with self.lock:
            for w in list(self.clients):
                if not self._send_now(w, b):
                    self.clients.remove(w)

    # ---- TCP ----
    def _hello(self, conn, addr, line):
        """인증 전 첫 줄. 통과하면 clients 에 넣고 True."""
        if line.startswith("hello ") and self._accept_token(line[6:]):
            with self.lock:
                if len(self.clients) >= MAX_CLIENTS:
                    self._send_now(conn, b"busy\n")
                    return False
                self._send_now(conn, ("ok %s\n" % self.id).encode())   # 등록 전에 보내 다른 줄과 섞이지 않게
                self.clients.append(conn)
            self.log(f"[lan] 드라이버 연결 {addr[0]}")
            return True
        time.sleep(DENY_DELAY)                       # 토큰 맞히기 늦추기
        self._send_now(conn, b"denied\n")
        self.log(f"[lan] 토큰 거절 {addr[0]} (페어링 필요: LAN_PAIR=1 로 재시작)")
        return False

    def _client(self, conn, addr):
        conn.settimeout(1)                           # 인증 전엔 1초마다 깨어 5초 기한을 본다
        authed, pre = False, True                    # pre = 인증 전 연결 수에 들어가 있음
        buf = b""
        t0 = last_ping = time.monotonic()
        try:
            while True:
                if not authed and time.monotonic() - t0 > HELLO_SEC:   # 조금씩 흘려 보내도 5초면 끝
                    break
                try:
                    data = conn.recv(512)
                    if not data:
                        break
                    buf += data
                except socket.timeout:
                    pass
                except OSError:
                    break
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip().decode("latin1")
                    if not authed:
                        if not self._hello(conn, addr, line):
                            return
                        authed = True
                        with self.lock:
                            self.preauth -= 1
                        pre = False
                        conn.settimeout(30)
                        self.core.command("refresh")      # 지금 상태 바로
                    else:
                        parts = line.split(" ", 1)
                        if len(parts) == 2 and len(parts[0]) == 2 and all(c in _HEX for c in parts[0]):
                            cmd = int(parts[0], 16)
                            try:
                                body = bytes.fromhex(parts[1]) if parts[1] else b""
                            except ValueError:
                                continue
                            if cmd == 0x00:                   # CMD_REQUEST
                                self.core.command(body.decode("latin1"))
                if len(buf) >= MAX_LINE:                 # 너무 긴 줄: 인증 전이면 끊고, 뒤면 버림
                    if not authed:
                        break
                    buf = b""
                if authed and time.monotonic() - last_ping > 30:
                    last_ping = time.monotonic()
                    with self.lock:
                        if not self._send_now(conn, b"ping\n"):
                            break
        finally:
            with self.lock:
                if conn in self.clients:
                    self.clients.remove(conn)
                if pre:
                    self.preauth -= 1
            try:
                conn.close()
            except Exception:
                pass
            if authed:
                self.log(f"[lan] 드라이버 끊김 {addr[0]}")

    def _tcp_server(self):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("0.0.0.0", self.port))
        s.listen(4)
        self.log(f"[lan] 통로 대기 TCP {self.port}")
        while True:
            try:
                c, a = s.accept()
            except OSError as e:
                self.log(f"[lan] accept 오류: {e}")
                time.sleep(1)
                continue
            with self.lock:
                room = self.preauth < MAX_PREAUTH
                if room:
                    self.preauth += 1
            if not room:                                 # 인증 전 연결이 꽉 참: 바로 끊음
                c.close()
                continue
            try:
                threading.Thread(target=self._client, args=(c, a), daemon=True).start()
            except RuntimeError as e:
                with self.lock:
                    self.preauth -= 1
                c.close()
                self.log(f"[lan] 스레드 못 만듦: {e}")

    # ---- SSDP ----
    def _ssdp_server(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("0.0.0.0", 1900))
            mreq = struct.pack("4sl", socket.inet_aton("239.255.255.250"), socket.INADDR_ANY)
            s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        except OSError as e:
            self.log(f"[lan] SSDP 못 엶({e}) — 드라이버가 못 찾으면 수동 IP 로")
            return
        self.log(f"[lan] SSDP 응답 준비 ({self.nas_ip})")
        while True:
            try:
                data, frm = s.recvfrom(1024)
            except OSError:
                continue
            if not data.startswith(b"M-SEARCH") or (SSDP_ST.encode() not in data and b"ssdp:all" not in data):
                continue
            rsp = (
                "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
                f"LOCATION: http://{self.nas_ip}/\r\nSERVER: lynnai-nas/1\r\n"
                f"ST: {SSDP_ST}\r\nUSN: uuid:lynnai-{self.id}::{SSDP_ST}\r\n"
                f"LYNNAI-ID: {self.id}\r\nLYNNAI-PORT: {self.port}\r\n\r\n"
            ).encode()
            try:
                s.sendto(rsp, frm)
            except OSError:
                pass

    def start(self):
        self.core.add_sender(self.send)
        threading.Thread(target=self._tcp_server, daemon=True).start()
        threading.Thread(target=self._ssdp_server, daemon=True).start()
