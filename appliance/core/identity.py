"""
identity.py -- 기기 개별화의 뿌리. "공유 비밀 0" 원칙의 핵심.

배포 이미지는 전부 동일하게 구워진다. 각 기기는 첫 부팅에 자신의 CPU 시리얼
(고유)과 HW RNG로 '자기만의' 비밀을 만든다. 따라서 이미지 한 장을 뜯어봐도
다른 기기는 안전하다.

초기 AP 암호는 '시리얼에서 결정적으로 파생' → 제조 시 시리얼만 알면 스티커를
미리 인쇄할 수 있다(소유자가 첫 설정에서 교체 강제).

CPython / (대부분) MicroPython 공용을 지향. os.urandom / hashlib 만 사용.
"""
import hashlib
import hmac
import os
import re

# 이미지에 구워지는 '비-비밀' 앱 상수. device_id(=SSID 접미사) 파생용.
# AP 암호 파생엔 쓰지 않는다(동일 이미지라 파생값은 비밀이 못 됨) — gen_ap_pass 참고.
ID_DERIVE_SALT = b"rk-appliance/device-id/v1"

# 혼동 문자(0/O, 1/l/I) 제거한 base32 유사 알파벳
_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"


def _b32(data, n):
    """bytes → 모호하지 않은 대문자/숫자 n글자."""
    out = []
    acc = 0
    bits = 0
    for b in data:
        acc = (acc << 8) | b
        bits += 8
        while bits >= 5:
            bits -= 5
            out.append(_ALPHABET[(acc >> bits) & 0x1F])
            if len(out) >= n:
                return "".join(out)
    while len(out) < n:
        out.append(_ALPHABET[(acc << (5 - bits)) & 0x1F])
        bits = 5
    return "".join(out[:n])


def read_cpu_serial():
    """기기 고유 ID. RPi 우선순위대로 시도, 없으면 machine-id, 최후엔 랜덤."""
    # 1) /proc/cpuinfo 의 Serial (RPi)
    try:
        with open("/proc/cpuinfo") as f:
            m = re.search(r"Serial\s*:\s*([0-9a-fA-F]+)", f.read())
            if m and set(m.group(1)) != {"0"}:
                return m.group(1).lower()
    except Exception:
        pass
    # 2) device-tree serial-number
    try:
        with open("/sys/firmware/devicetree/base/serial-number", "rb") as f:
            s = f.read().strip(b"\x00").decode("ascii", "ignore").strip()
            if s:
                return s.lower()
    except Exception:
        pass
    # 3) /etc/machine-id
    try:
        with open("/etc/machine-id") as f:
            s = f.read().strip()
            if s:
                return s.lower()
    except Exception:
        pass
    # 4) 최후: 랜덤(비결정적 — 재부팅마다 바뀌면 안 되므로 호출측이 저장해야 함)
    return os.urandom(8).hex()


def device_id(serial):
    """시리얼 → 짧고 안정적인 공개 기기 ID (SSID 접미사 등)."""
    h = hmac.new(ID_DERIVE_SALT, serial.encode(), hashlib.sha256).digest()
    return _b32(h, 6)


def gen_ap_pass(n=12):
    """생산용 AP 암호를 '진짜 랜덤'으로 생성(첫 프로비저닝 때). HW RNG 기반.
    시리얼 파생이 아님 — 이미지가 동일 배포라 파생값은 비밀이 못 되기 때문.
    모호하지 않은 대문자/숫자 n글자(≈60비트). 포털이 1회 표시 후 재연결 유도."""
    return _b32(os.urandom(32), n)


def gen_secret(nbytes=16):
    """진짜 랜덤 비밀(관리자 솔트, 세션 토큰 등). HW RNG 기반."""
    return os.urandom(nbytes).hex()


def ap_ssid(serial, base="LYNNAI"):
    return "%s-%s" % (base, device_id(serial))


if __name__ == "__main__":
    s = read_cpu_serial()
    print("serial       :", s)
    print("device_id    :", device_id(s))
    print("ap_ssid      :", ap_ssid(s))
    print("gen_ap_pass  :", gen_ap_pass(), "(랜덤 — 첫 프로비저닝용, 매번 다름)")
    print("gen_ap_pass2 :", gen_ap_pass(), "(달라야 함)")
    print("gen_secret   :", gen_secret(), "(매번 달라야 함)")
