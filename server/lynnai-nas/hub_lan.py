"""
hub_lan.py (대응 ESP32: hub_lan.c) -- SmartThings LAN Edge 드라이버가 붙는 통로. ESP32 펌웨어 hub_lan.c 의 파이썬 판.
드라이버(edge-driver-lan)는 Zigbee 든 LAN 이든 같은 core.lua 를 쓰므로, 이 통로만 열면 그대로 붙는다.

찾기(SSDP): 239.255.255.250:1900 의 M-SEARCH(ST: urn:lynnai-com:device:hub:1 / ssdp:all)에
  HTTP/1.1 200 OK + LYNNAI-ID + LYNNAI-PORT 로 답한다.
연결(TCP 8484, 줄 단위):
  드라이버 → "hello <토큰32hex>"   허브 → "ok <id>" | "denied" | "busy"
  이후 양쪽 "<명령 16진 2자리> <내용 16진>\n",  허브는 30초마다 "ping"
토큰: 저장된 게 없으면 처음 붙는 드라이버를 믿고 저장(첫 설치). 그 뒤 새 드라이버는 pairing_open() 창에서만.
"""
import json
import os
import secrets
import socket
import struct
import threading
import time

SSDP_ST = "urn:lynnai-com:device:hub:1"
MAX_CLIENTS = 2


class LanUplink:
    def __init__(self, core, hub_id, nas_ip, data_dir, port=8484, log=print):
        self.core = core
        self.id = hub_id
        self.nas_ip = nas_ip
        self.port = port
        self.log = log
        self.tok_path = os.path.join(data_dir, "lan_tokens.json")
        self.tokens = self._load_tokens()
        self.pair_until = 0
        self.clients = []           # 인증된 writer(파일류)
        self.lock = threading.Lock()

    # ---- 토큰 ----
    def _load_tokens(self):
        try:
            return set(json.load(open(self.tok_path)))
        except Exception:
            return set()

    def _save_tokens(self):
        try:
            json.dump(sorted(self.tokens), open(self.tok_path, "w"))
        except Exception as e:
            self.log(f"[lan] 토큰 저장 실패: {e}")

    def pairing_open(self, seconds=180):
        self.pair_until = time.time() + seconds
        self.log(f"[lan] 페어링 {seconds}초 열림")

    def _accept_token(self, tok):
        if len(tok) != 32 or not all(c in "0123456789abcdef" for c in tok):
            return False
        with self.lock:
            if tok in self.tokens:
                return True
            if not self.tokens or time.time() < self.pair_until:
                self.tokens.add(tok)
                self._save_tokens()
                self.log("[lan] 새 드라이버 등록(%s)" % ("첫 설치" if len(self.tokens) == 1 else "페어링"))
                return True
        return False

    # ---- 내보내기(core → 모든 드라이버) ----
    def send(self, cmd_id, data):
        line = ("%02x " % cmd_id) + data.hex() + "\n"
        b = line.encode()
        with self.lock:
            dead = []
            for w in self.clients:
                try:
                    w.sendall(b)
                except Exception:
                    dead.append(w)
            for w in dead:
                self.clients.remove(w)

    # ---- TCP ----
    def _client(self, conn, addr):
        conn.settimeout(5)
        authed = False
        buf = b""
        last_ping = time.time()
        try:
            while True:
                try:
                    data = conn.recv(512)
                    if not data:
                        break
                    buf += data
                except socket.timeout:
                    data = b""
                except OSError:
                    break
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip().decode("latin1")
                    if not authed:
                        if line.startswith("hello ") and self._accept_token(line[6:]):
                            with self.lock:
                                if len(self.clients) >= MAX_CLIENTS:
                                    conn.sendall(b"busy\n")
                                    return
                                conn.sendall(("ok %s\n" % self.id).encode())
                                self.clients.append(conn)
                            authed = True
                            conn.settimeout(30)
                            self.log(f"[lan] 드라이버 연결 {addr[0]}")
                            self.core.command("refresh")      # 지금 상태 바로
                        else:
                            conn.sendall(b"denied\n")
                            self.log(f"[lan] 토큰 거절 {addr[0]} (페어링 필요)")
                            return
                    else:
                        parts = line.split(" ", 1)
                        if len(parts) == 2 and all(c in "0123456789abcdef" for c in parts[0]):
                            cmd = int(parts[0], 16)
                            body = bytes.fromhex(parts[1]) if parts[1] else b""
                            if cmd == 0x00:                   # CMD_REQUEST
                                self.core.command(body.decode("latin1"))
                if authed and time.time() - last_ping > 30:
                    last_ping = time.time()
                    try:
                        conn.sendall(b"ping\n")
                    except OSError:
                        break
        finally:
            with self.lock:
                if conn in self.clients:
                    self.clients.remove(conn)
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
            c, a = s.accept()
            threading.Thread(target=self._client, args=(c, a), daemon=True).start()

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
