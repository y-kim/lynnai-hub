"""
state.py -- 기기 상태(device.json) + 첫부팅 개별화 + 관리자 암호 해시.

device.json 은 '그 기기에서 생성된' 값만 담는다(공유비밀 아님). SD를 읽으면
이 기기 값은 보이지만 — 그 공격자는 이미 이 집에 들어온 사람이고, 다른 기기는
각자 다른 값이라 안전하다(identity.py 참고).

관리자 암호는 PBKDF2-HMAC-SHA256(기기별 랜덤 솔트)로 '해시만' 저장. 평문 없음.
"""
import hashlib
import hmac
import json
import os
import time

import identity

SCHEMA = 1
PBKDF2_ITERS = 200_000

# proxy 모드에서 "진짜 클라우드로 중계 + 앱도 동작"시키는 데 필요한 값들.
# 앱(모바일)에 박혀 있거나 사용자 계정에 속한 것. standalone 에선 전부 불필요.
# 비-비밀(그대로 표시) / 비밀(마스킹) 구분은 PROXY_SECRET_KEYS.
PROXY_FIELDS = {
    "upstream_host": "www.rkiot.net",   # 진짜 클라우드 호스트명
    "upstream_ip": "",                  # 그 실제 IP(우리 DNS 하이재킹 우회용). 비우면 upstream_host DNS 조회
    "upstream_sni": "www.rkiot.net",    # TLS SNI
    "app_key": "",                      # 앱에 박힌 API 키(REST HMAC)
    "app_secret": "",                   # 앱 시크릿(있으면)
    "account_user": "",                 # 사용자 린나이 계정 ID
    "account_pass": "",                 # 사용자 린나이 계정 암호
    "mqtt_uid": "",                     # 벤더 MQTT uid(비우면 부트스트랩서 자동 획득)
    "mqtt_pass": "",                    # 벤더 MQTT pass
}
PROXY_SECRET_KEYS = {"app_key", "app_secret", "account_pass", "mqtt_pass"}


def _pbkdf2(password, salt_hex):
    return hashlib.pbkdf2_hmac(
        "sha256", password.encode(), bytes.fromhex(salt_hex), PBKDF2_ITERS
    ).hex()


class DeviceState:
    def __init__(self, path):
        self.path = path
        self.data = {}

    # ---- 로드/저장 ----
    def load(self):
        with open(self.path) as f:
            self.data = json.load(f)
        return self

    def save(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.data, f, indent=2, ensure_ascii=False)
        os.replace(tmp, self.path)          # 원자적 교체
        try:
            os.chmod(self.path, 0o600)
        except Exception:
            pass

    def exists(self):
        return os.path.exists(self.path)

    # ---- 첫부팅 개별화 ----
    def first_boot(self, defaults):
        """device.json 이 없으면 identity + defaults 로 생성(멱등)."""
        if self.exists():
            self.load()
            return False
        serial = identity.read_cpu_serial()
        did = identity.new_device_id()
        self.data = {
            "schema": SCHEMA,
            "created": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "device_id": did,
            "cpu_serial": serial,
            # --- 프로비저닝 상태 ---
            # 미프로비저닝: 공개 setup AP로 뜬다(암호는 이미지에 박힌 공개 기본값 = 비밀 아님,
            # 링크 암호화만 제공). 첫 접속 포털에서 생산 SSID+WPA와 관리자암호 설정 후 전환.
            "provisioned": False,
            "setup_ssid": "%s-%s" % (defaults.get("setup_ssid_base", "LYNNAI-SETUP"), did),
            "setup_pass": defaults.get("setup_pass", ""),        # "" = 개방, 아니면 공개 WPA2 기본값
            "ap_ssid_default": "%s-%s" % (defaults.get("ap_ssid_base", "LYNNAI-HUB"), did),  # 폼 기본값
            "ap_ssid": None,                    # 생산 SSID — 프로비저닝서 유저가 정함
            "ap_pass": None,                    # 생산 WPA2 암호 — 랜덤 생성 or 유저 지정
            "admin": None,                      # 프로비저닝서 설정 강제
            "mode": defaults.get("default_mode", "standalone"),  # B
            "uplink": None,                     # standalone 기본: 업링크 없음
            "update_pubkey": defaults.get("update_pubkey", ""),  # 서명검증 공개키(공개 안전)
            "session_secret": identity.gen_secret(16),           # 포털 세션 서명키
            "proxy": dict(PROXY_FIELDS),                         # proxy 모드 전용 자격증명(기본 빈값)
        }
        self.save()
        return True

    # ---- 관리자 암호 ----
    def admin_is_set(self):
        return bool(self.data.get("admin"))

    def set_admin(self, password):
        if len(password) < 8:
            raise ValueError("관리자 암호는 8자 이상")
        salt = identity.gen_secret(16)
        self.data["admin"] = {"algo": "pbkdf2_sha256", "iters": PBKDF2_ITERS,
                              "salt": salt, "hash": _pbkdf2(password, salt)}
        self.save()

    def verify_admin(self, password):
        a = self.data.get("admin")
        if not a:
            return False
        try:
            calc = _pbkdf2(password, a["salt"])
        except Exception:
            return False
        # 상수시간 비교
        return hmac.compare_digest(calc, a["hash"])

    # ---- 프로비저닝(공개 setup → 생산 AP) ----
    def is_provisioned(self):
        return bool(self.data.get("provisioned"))

    def current_ap(self):
        """지금 hostapd가 띄워야 할 (ssid, pass). pass=None 이면 개방 AP(설정창에만 해당)."""
        if self.is_provisioned():
            return self.data["ap_ssid"], self.data["ap_pass"]
        return self.data["setup_ssid"], (self.data.get("setup_pass") or None)

    def provision(self, admin_pw, ssid, ap_pass=None):
        """첫 설정: 관리자암호 + 생산 SSID + 생산 WPA2암호를 한 번에 확정.
        ap_pass 미지정이면 랜덤 생성. 생성/지정된 암호를 반환(포털이 1회 표시)."""
        if self.is_provisioned():
            raise RuntimeError("이미 프로비저닝됨")
        ssid = (ssid or "").strip()
        if not (1 <= len(ssid) <= 32):
            raise ValueError("SSID는 1~32자")
        if ap_pass:
            if len(ap_pass) < 8:
                raise ValueError("AP 암호는 8자 이상(WPA2)")
        else:
            ap_pass = identity.gen_ap_pass()
        # admin 먼저 검증(짧으면 여기서 예외 → 아무것도 안 바뀜)
        if len(admin_pw) < 8:
            raise ValueError("관리자 암호는 8자 이상")
        salt = identity.gen_secret(16)
        self.data["admin"] = {"algo": "pbkdf2_sha256", "iters": PBKDF2_ITERS,
                              "salt": salt, "hash": _pbkdf2(admin_pw, salt)}
        self.data["ap_ssid"] = ssid
        self.data["ap_pass"] = ap_pass
        self.data["provisioned"] = True
        self.save()
        return ap_pass

    # ---- AP 암호 교체(프로비저닝 후, 소유자) ----
    def set_ap_pass(self, new_pass):
        if not self.is_provisioned():
            raise RuntimeError("프로비저닝 전")
        if len(new_pass) < 8:
            raise ValueError("AP 암호는 8자 이상(WPA2)")
        self.data["ap_pass"] = new_pass
        self.save()

    def set_ap_ssid(self, new_ssid):
        if not self.is_provisioned():
            raise RuntimeError("프로비저닝 전")
        new_ssid = (new_ssid or "").strip()
        if not (1 <= len(new_ssid) <= 32):
            raise ValueError("SSID는 1~32자")
        self.data["ap_ssid"] = new_ssid
        self.save()

    # ---- 업링크(옵션; standalone 기본은 None) ----
    def set_uplink(self, ssid, psk):
        self.data["uplink"] = {"ssid": ssid, "psk": psk}
        self.save()

    def clear_uplink(self):
        self.data["uplink"] = None
        self.save()

    def set_mode(self, mode):
        if mode not in ("standalone", "bus", "proxy"):
            raise ValueError("unknown mode")
        self.data["mode"] = mode
        self.save()

    # ---- proxy 자격증명(옵트인) ----
    def get_proxy(self):
        """현재 proxy 설정(기본 필드와 병합). 비밀 포함 — 표시용은 proxy_view() 사용."""
        p = dict(PROXY_FIELDS)
        p.update(self.data.get("proxy") or {})
        return p

    def set_proxy(self, fields):
        """빈 문자열은 '건드리지 않음'으로 취급(비밀 보존). 명시적 삭제는 clear_proxy_key."""
        cur = self.get_proxy()
        for k, v in fields.items():
            if k not in PROXY_FIELDS:
                continue
            if v == "" and k in PROXY_SECRET_KEYS:
                continue                      # 비밀칸 공백 = 기존 유지
            cur[k] = v
        self.data["proxy"] = cur
        self.save()

    def clear_proxy_key(self, key):
        if key in PROXY_FIELDS:
            p = self.get_proxy()
            p[key] = ""
            self.data["proxy"] = p
            self.save()

    def proxy_view(self):
        """포털 표시용: 비밀은 값 대신 설정여부만. 비-비밀은 그대로."""
        p = self.get_proxy()
        out = {}
        for k in PROXY_FIELDS:
            if k in PROXY_SECRET_KEYS:
                out[k] = {"set": bool(p.get(k)), "secret": True}
            else:
                out[k] = {"value": p.get(k, ""), "secret": False}
        return out

    # ---- 공장초기화: per-device 비밀 소거(양도 대비) ----
    def factory_reset(self):
        os.remove(self.path)
        self.data = {}


if __name__ == "__main__":
    import tempfile
    p = os.path.join(tempfile.mkdtemp(), "device.json")
    st = DeviceState(p)
    created = st.first_boot({"default_mode": "standalone", "setup_ssid_base": "LYNNAI-SETUP"})
    print("first_boot created:", created, " provisioned:", st.is_provisioned())
    print("setup AP (미프로비저닝):", st.current_ap())
    # 프로비저닝: 관리자+생산SSID, 암호는 랜덤생성
    genpw = st.provision("hunter2pw", "OurHome-Boiler")
    print("provisioned:", st.is_provisioned(), " 생성된 AP암호:", genpw)
    print("production AP:", st.current_ap())
    print("admin verify ok:", st.verify_admin("hunter2pw"), " bad:", st.verify_admin("x"))
    # 재프로비저닝 거부
    try:
        st.provision("x", "y"); print("❌ 재프로비저닝 허용됨")
    except RuntimeError:
        print("재프로비저닝 거부 ✓")
    # AP암호 교체
    st.set_ap_pass("myNewApPass9"); print("ap_pass 교체:", st.data["ap_pass"])
    # 재로드 멱등(이미 provisioned면 first_boot는 로드만)
    st2 = DeviceState(p); again = st2.first_boot({})
    print("reload: created =", again, "(False여야), ssid 보존:", st2.data["ap_ssid"] == "OurHome-Boiler")
    print("OK", p)
