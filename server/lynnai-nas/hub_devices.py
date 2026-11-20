"""
hub_devices.py (대응 ESP32: 버튼을 길게 눌러 여는 기기 등록 창) -- 린나이 기기 등록부: 후보 → 승인.

등록 안 된 출발지가 린나이 기기처럼 굴면(가로채는 이름을 NAS 의 DNS 에 물음 / Ck-Mac 을 달고 /key/server 를 부름)
후보 표에 적어 두고, 사람이 승인하면(hub_api 의 /devices·/api/v1/devices/approve) 등록부로 옮긴다.
DEVICE_PAIR=1 로 시작하면 처음 600초(등록 창) 동안은 후보가 보이는 그 자리에서 등록한다(LAN_PAIR 와 같은 방식).

  후보   : 메모리에만. 최대 MAX_CAND 개(넘으면 가장 오래 전에 본 것부터 버림).
           IP, MAC(호스트 ARP 표 — 모를 수 있음), 처음·마지막 본 시각, 횟수, 경로(dns/http).
  등록부 : DATA_DIR/devices.json (0600, 임시 파일에 쓰고 바꿔치기). 최대 MAX_DEVICES 대.
           mac(소문자, 콜론. ARP 를 못 읽었으면 빈 값), ip(마지막으로 본 주소), added, note.

판정(is_registered): 그 IP 의 MAC 을 ARP 표(/proc/net/arp, ARP_SEC 초 재사용)에서 찾아 등록된 MAC 이면 등록된 기기다
(주소가 바뀌었으면 마지막 주소를 고쳐 적는다). MAC 을 못 찾으면(ARP 표에 없음·못 읽음) 마지막 주소가 같은 항목으로,
MAC 을 찾았으면 MAC 이 빈 항목 중 주소가 같은 것으로 본다. ARP 표를 아예 못 읽으면(리눅스가 아님 등) 경고를 한 번 남기고
IP 로만 판정한다.

MAC 도 IP 도 같은 LAN 에서는 꾸밀 수 있다 — RINNAI_IPS 목록과 같은 수준의 믿음이고, 승인은 사람이 판단한다.

락: self.lock 은 메모리(등록부·후보·ARP 캐시)만 잠깐 쥔다. 파일·ARP 읽기·로그는 이 락 밖에서 한다.
    파일 쓰기와 사람이 하는 승인·삭제는 self._io 로 줄을 세운다(DNS 스레드는 _io 를 잡지 않는다 — 자동 등록과
    주소 갱신의 저장은 따로 스레드에서).
"""
import ipaddress
import json
import os
import re
import threading
import time

MAX_DEVICES = 8
MAX_CAND = 16
ARP_PATH = "/proc/net/arp"
ARP_SEC = 30                    # ARP 표를 다시 읽는 간격(패킷마다 읽지 않게)
ARP_MISS_SEC = 3                # 표에 없는 주소를 물었을 때는 이만큼만 지나도 다시 읽는다(방금 켠 기기)
MOVE_SAVE_SEC = 10              # 주소가 바뀐 것을 파일에 적는 최소 간격(같은 MAC 이 두 주소를 오가도 계속 쓰지 않게)
PAIR_SEC = 600
NOTE_MAX = 64
RINNAI_OUI = "34:5f:45"         # 린나이 Wi-Fi 모듈의 MAC 앞자리
LOG_PER_MIN = 16                # 새 후보 로그 상한(출발지를 바꿔 가며 물어도 로그가 쏟아지지 않게)
_HEX12 = re.compile(r"[0-9a-f]{12}")


def norm_mac(s):
    """MAC 을 소문자 콜론 꼴로. 형식이 아니면 ""."""
    if not isinstance(s, str):
        return ""
    h = s.strip().lower().replace(":", "").replace("-", "").replace(".", "")
    if not _HEX12.fullmatch(h):
        return ""
    return ":".join(h[i:i + 2] for i in range(0, 12, 2))


def norm_ip(s):
    """IPv4 주소를 표준 꼴로. 아니면 ""."""
    if not isinstance(s, str):
        return ""
    try:
        return str(ipaddress.IPv4Address(s.strip()))
    except ValueError:
        return ""


def _note(s):
    if not isinstance(s, str):
        return ""
    return "".join(ch for ch in s if ch.isprintable())[:NOTE_MAX].strip()


def read_arp(path=ARP_PATH):
    """호스트 ARP 표 → {ip: mac}. 다 풀린 항목(Flags 0x2)만. 못 읽으면 OSError."""
    out = {}
    with open(path) as f:
        lines = f.read(1 << 20).splitlines()
    for line in lines[1:]:
        p = line.split()
        if len(p) < 4:
            continue
        try:
            flags = int(p[2], 16)
        except ValueError:
            continue
        mac = norm_mac(p[3])
        if flags & 0x2 and mac and mac != "00:00:00:00:00:00":
            out.setdefault(p[0], mac)
    return out


class Devices:
    def __init__(self, data_dir, log=print, arp=read_arp, ignore=()):
        """arp: () → {ip: mac} (시험에서 바꿔 넣는다). ignore: 후보로 적지 않을 주소(NAS 자신)."""
        self.path = os.path.join(data_dir, "devices.json")
        self.log = log
        self.ignore = set(ignore)
        self.lock = threading.Lock()
        self._io = threading.Lock()
        self._arp_read = arp
        self._arp = {}
        self._arp_at = None          # 마지막으로 읽은 시각(monotonic). None = 아직 안 읽음
        self._arp_ok = True
        self._arp_warned = False
        self.cand = {}               # ip -> 후보
        self.pair_until = 0
        self._moved = -MOVE_SAVE_SEC
        self._log_t, self._log_n = 0.0, 0
        self.reg = self._load()

    # ---- 파일 ----
    def _load(self):
        try:
            with open(self.path) as f:
                v = json.load(f)
            items = v["devices"] if isinstance(v, dict) else None
            if not isinstance(items, list):
                raise ValueError("형식이 다름")
        except FileNotFoundError:
            return []
        except Exception as e:
            try:                                     # 다음 저장이 덮어쓰기 전에 옆에 남겨 둔다
                os.replace(self.path, self.path + ".bad")
            except OSError:
                pass
            self.log(f"[dev] 등록부 파일을 못 읽음({e}) — 빈 등록부로 시작(원래 파일은 devices.json.bad)")
            return []
        out, seen, skipped = [], set(), 0
        for it in items:
            if not isinstance(it, dict):
                skipped += 1
                continue
            mac, ip = norm_mac(it.get("mac", "")), norm_ip(it.get("ip", ""))
            key = mac or ip
            if not key or (it.get("mac") and not mac) or key in seen or len(out) >= MAX_DEVICES:
                skipped += 1
                continue
            seen.add(key)
            added = it.get("added")
            out.append({"mac": mac, "ip": ip, "added": int(added) if isinstance(added, (int, float)) else 0,
                        "note": _note(it.get("note", ""))})
        if skipped:
            self.log(f"[dev] 등록부 파일의 항목 {skipped}개를 버림(형식이 다르거나 겹치거나 {MAX_DEVICES}대 초과)")
        return out

    def _write(self):
        """지금 등록부를 파일로(_io 를 쥔 채 부른다)."""
        with self.lock:
            data = {"v": 1, "devices": [dict(e) for e in self.reg]}
        tmp = self.path + ".tmp"
        try:
            fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w") as f:
                if hasattr(os, "fchmod"):
                    os.fchmod(f.fileno(), 0o600)     # 남아 있던 임시 파일의 권한이 달라도
                json.dump(data, f, ensure_ascii=False, indent=1)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp, self.path)
            return True
        except Exception as e:
            self.log(f"[dev] 등록부 저장 실패: {e}")
            return False

    def _save_bg(self, what):
        """DNS 스레드에서 디스크를 기다리지 않게 따로 저장한다. 못 쓰면 메모리에만 남는다(로그)."""
        def run():
            with self._io:
                if not self._write():
                    self.log(f"[dev] {what} 을(를) 파일에 못 적음 — 다시 시작하면 사라짐")
        try:
            threading.Thread(target=run, daemon=True).start()
        except RuntimeError as e:
            self.log(f"[dev] 저장 스레드 못 만듦({e}) — {what} 은(는) 메모리에만")

    # ---- ARP ----
    def mac_of(self, ip, fresh=False):
        """그 IP 의 MAC(ARP 표). 모르면 ""."""
        now = time.monotonic()
        with self.lock:
            if self._arp_at is not None and not fresh:
                age = now - self._arp_at
                mac = self._arp.get(ip, "")
                if age < (ARP_MISS_SEC if self._arp_ok else ARP_SEC) or (mac and age < ARP_SEC):
                    return mac
        try:
            tab, err = self._arp_read(), None
            if not isinstance(tab, dict):
                raise ValueError("ARP 표 형식이 다름")
        except Exception as e:
            tab, err = {}, e
        with self.lock:
            self._arp, self._arp_at, self._arp_ok = tab, time.monotonic(), err is None
            warn = err is not None and not self._arp_warned
            if warn:
                self._arp_warned = True
        if warn:
            self.log(f"[dev] 경고: ARP 표를 못 읽음({err}) — MAC 을 모르므로 등록 기기를 IP 로만 판정"
                     "(host 네트워크가 아니거나 리눅스가 아님)")
        return tab.get(ip, "")

    # ---- 판정 ----
    def _find(self, ip, mac):
        """등록부에서 이 출발지에 해당하는 항목(self.lock 을 쥔 채)."""
        if mac:
            for e in self.reg:
                if e["mac"] == mac:
                    return e
            for e in self.reg:
                if not e["mac"] and e["ip"] == ip:
                    return e
            return None
        for e in self.reg:
            if e["ip"] == ip:
                return e
        return None

    def is_registered(self, ip, learn=True):
        """등록된 기기인가. learn 이면 등록된 MAC 이 새 주소에서 보일 때 마지막 주소를 고쳐 적는다."""
        mac = self.mac_of(ip)
        moved = None
        with self.lock:
            e = self._find(ip, mac)
            if e is None:
                return False
            if learn and mac and e["mac"] == mac and e["ip"] != ip:
                old, e["ip"] = e["ip"], ip
                now = time.monotonic()
                if now - self._moved >= MOVE_SAVE_SEC:
                    self._moved = now
                    moved = old
        if moved is not None:
            self.log(f"[dev] 등록 기기 {mac} 주소 바뀜 {moved or '(모름)'} → {ip}")
            self._save_bg(f"{mac} 의 새 주소")
        return True

    # ---- 후보 ----
    def pairing_open(self, seconds=PAIR_SEC):
        self.pair_until = time.monotonic() + seconds
        self.log(f"[dev] 기기 등록 창 {seconds}초 열림 — 이 동안 보이는 후보는 바로 등록")

    def _may_log(self):
        """새 후보 로그를 남겨도 되나(self.lock 을 쥔 채)."""
        now = time.monotonic()
        if now - self._log_t > 60:
            self._log_t, self._log_n = now, 0
        self._log_n += 1
        return self._log_n <= LOG_PER_MIN

    def seen(self, ip, via="dns"):
        """등록 안 된 출발지가 린나이 기기처럼 굴었다 → 후보로 적는다. 등록 창이 열려 있으면 그 자리에서 등록.
        돌려주는 값: 지금 등록된 기기인가(자동 등록됐거나 그 사이 승인됨)."""
        ip = norm_ip(ip)
        if not ip or ip in self.ignore:
            return False
        mac = self.mac_of(ip)
        now = time.time()
        new = False
        with self.lock:
            if self._find(ip, mac) is not None:
                return True
            c = self.cand.get(ip)
            if c is not None and mac and c["mac"] and c["mac"] != mac:
                c = None                             # 같은 주소를 다른 기기가 씀 → 새 후보로
            if c is None:
                if ip not in self.cand and len(self.cand) >= MAX_CAND:
                    del self.cand[min(self.cand, key=lambda k: self.cand[k]["last"])]
                c = self.cand[ip] = {"ip": ip, "mac": mac, "first": now, "last": now, "count": 0, "via": via}
                new = True
            c["last"] = now
            c["count"] += 1
            if mac:
                c["mac"] = mac
            pairing = time.monotonic() < self.pair_until
            say = (new or pairing) and self._may_log()
        who = f"{ip} (MAC {mac or '모름'}{', 린나이 모듈로 보임' if mac.startswith(RINNAI_OUI) else ''})"
        if pairing:
            code, _e = self.approve(ip=ip, auto=True)
            if code in ("ok", "exists"):
                self.log(f"[dev] 등록 창: {who} 자동 등록")
                return True
            if say:
                self.log(f"[dev] 등록 창이 열려 있지만 {who} 을(를) 등록 못 함({_MSG.get(code, code)})")
            return False
        if say:
            self.log(f"[dev] 새 후보 {who}, 경로 {via} — /devices 에서 승인하면 등록")
        return False

    def _cand_for(self, ip, mac):
        """승인할 후보(self.lock 을 쥔 채). ip 가 있으면 그 주소, 없으면 그 MAC 으로 가장 최근에 본 것."""
        if ip:
            return self.cand.get(ip)
        best = None
        for c in self.cand.values():
            if c["mac"] == mac and (best is None or c["last"] > best["last"]):
                best = c
        return best

    def _drop_cand(self, ip, mac):
        for k in [k for k, c in self.cand.items() if k == ip or (mac and c["mac"] == mac)]:
            del self.cand[k]

    def _approve(self, ip, mac, note):
        """후보 하나를 등록부에 넣는다(메모리만). → (결과, 넣은 항목 / 이미 있던 항목의 사본 / None)."""
        with self.lock:
            c = self._cand_for(ip, mac)
            if c is None:
                return "notfound", None
            cip, cmac = c["ip"], c["mac"]
        if not cmac:
            cmac = self.mac_of(cip, fresh=True)      # 볼 때는 몰랐어도 지금은 ARP 표에 있을 수 있다
        e = {"mac": cmac, "ip": cip, "added": int(time.time()), "note": _note(note)}
        with self.lock:
            cur = self._find(cip, cmac)
            if cur is not None:
                self._drop_cand(cip, cmac)
                return "exists", dict(cur)
            if len(self.reg) >= MAX_DEVICES:
                return "full", None
            self.reg.append(e)
            return "ok", e

    def approve(self, ip="", mac="", note="", auto=False):
        """후보를 등록부로. ip 또는 mac 으로 고른다. → (결과, 항목 사본 또는 None)
        결과: ok / exists(이미 등록됨) / notfound(후보에 없음) / full / bad(값이 틀림) / savefail.
        auto(등록 창)면 저장을 기다리지 않는다. 사람이 승인할 때는 저장까지 되어야 등록한다(다시 시작해도 남게)."""
        ip_n, mac_n = norm_ip(ip) if ip else "", norm_mac(mac) if mac else ""
        if (ip and not ip_n) or (mac and not mac_n) or not (ip_n or mac_n):
            return "bad", None
        if auto:
            code, e = self._approve(ip_n, mac_n, note)
            if code == "ok":
                with self.lock:
                    self._drop_cand(e["ip"], e["mac"])
                self._save_bg(f"자동 등록한 {e['mac'] or e['ip']}")
            return code, dict(e) if e else None
        with self._io:
            code, e = self._approve(ip_n, mac_n, note)
            if code != "ok":
                return code, e
            if not self._write():
                with self.lock:
                    self.reg[:] = [x for x in self.reg if x is not e]
                return "savefail", None
            with self.lock:
                self._drop_cand(e["ip"], e["mac"])
        self.log(f"[dev] 기기 등록(승인): {e['ip']} MAC {e['mac'] or '모름 — IP 로 판정'}")
        return "ok", dict(e)

    def remove(self, ip="", mac=""):
        """등록부에서 뺀다. mac(있으면 먼저) 또는 마지막 주소 ip 로 고른다. → (결과, 뺀 항목 사본 또는 None)"""
        ip_n, mac_n = norm_ip(ip) if ip else "", norm_mac(mac) if mac else ""
        if (ip and not ip_n) or (mac and not mac_n) or not (ip_n or mac_n):
            return "bad", None
        with self._io:
            with self.lock:
                for i, e in enumerate(self.reg):
                    if (e["mac"] == mac_n) if mac_n else (e["ip"] == ip_n):
                        break
                else:
                    return "notfound", None
                del self.reg[i]
            if not self._write():
                with self.lock:
                    self.reg.insert(i, e)
                return "savefail", None
        self.log(f"[dev] 기기 등록 삭제: {e['ip'] or '(주소 모름)'} MAC {e['mac'] or '모름'}")
        return "ok", dict(e)

    def snapshot(self):
        """{registered, candidates(최근에 본 순), pairing_until(epoch, 닫혀 있으면 0)}"""
        left = self.pair_until - time.monotonic()
        with self.lock:
            reg = [dict(e) for e in self.reg]
            cand = [dict(c, first=int(c["first"]), last=int(c["last"]), rinnai=c["mac"].startswith(RINNAI_OUI))
                    for c in sorted(self.cand.values(), key=lambda c: -c["last"])]
        return {"registered": reg, "candidates": cand, "pairing_until": int(time.time() + left) if left > 0 else 0}


_MSG = {"ok": "등록함", "exists": "이미 등록된 기기", "notfound": "후보에 없음",
        "full": f"등록 기기가 {MAX_DEVICES}대로 꽉 참 — 안 쓰는 것을 먼저 삭제", "bad": "ip 또는 mac 값이 틀림",
        "savefail": "등록부 파일을 못 써서 바꾸지 않음(로그 확인)"}


def message(code, removing=False):
    """결과 코드 → 사람이 읽을 말(hub_api 가 쓴다)."""
    if removing:
        return {"ok": "삭제함", "notfound": "등록부에 없음"}.get(code) or _MSG.get(code, code)
    return _MSG.get(code, code)
