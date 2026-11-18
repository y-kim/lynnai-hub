"""
identity.py -- 기기 개별화의 뿌리. "공유 비밀 0" 원칙의 핵심.

배포 이미지는 전부 동일하게 구워진다. 각 기기는 첫 부팅에 자신의 CPU 시리얼
(고유)과 HW RNG로 '자기만의' 비밀을 만든다. 따라서 이미지 한 장을 뜯어봐도
다른 기기는 안전하다.

기기 ID 는 첫 부팅 때 HW 난수로 뽑는다(시리얼에서 파생하지 않음). 공장초기화하면
상태 파일과 함께 사라지고 다음 첫 부팅에 새로 뽑힌다 → 양도 후엔 다른 기기로 보이고,
주인이 바뀌어도 방송되는 SSID 로 같은 기기를 추적할 수 없다.

CPython / (대부분) MicroPython 공용을 지향. os.urandom 만 사용.
"""
import os
import re

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


def new_device_id():
    """공개 기기 ID (SSID 접미사 등): HW 난수 6글자(30비트). 첫 부팅/공장초기화 때마다 새로."""
    return _b32(os.urandom(8), 6)


def gen_ap_pass(n=12):
    """생산용 AP 암호를 '진짜 랜덤'으로 생성(첫 프로비저닝 때). HW RNG 기반.
    시리얼 파생이 아님 — 이미지가 동일 배포라 파생값은 비밀이 못 되기 때문.
    모호하지 않은 대문자/숫자 n글자(≈60비트). 포털이 1회 표시 후 재연결 유도."""
    return _b32(os.urandom(32), n)


def gen_secret(nbytes=16):
    """진짜 랜덤 비밀(관리자 솔트, 세션 토큰 등). HW RNG 기반."""
    return os.urandom(nbytes).hex()


def ap_ssid(did, base="LYNNAI-HUB"):
    return "%s-%s" % (base, did)


if __name__ == "__main__":
    s = read_cpu_serial()
    print("serial       :", s)
    d = new_device_id()
    print("device_id    :", d, "/ 다시:", new_device_id(), "(매번 달라야 함)")
    print("ap_ssid      :", ap_ssid(d))
    print("gen_ap_pass  :", gen_ap_pass(), "(랜덤 — 첫 프로비저닝용, 매번 다름)")
    print("gen_ap_pass2 :", gen_ap_pass(), "(달라야 함)")
    print("gen_secret   :", gen_secret(), "(매번 달라야 함)")
