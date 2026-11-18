"""
defaults.py -- 굽는 시점 config 로더. 디바이스 식별자(브랜드/토픽/식별자접두)의 단일 출처.

배포 이미지는 config/defaults.json 을 담고(비-비밀, 나중 수정 가능), 코드는 전부
여기서 값을 읽는다. 그래서 'lynnai'든 뭐든 한 곳만 바꾸면 전 계층에 반영된다.
first_boot / 포털 / (추후) 컨트롤 클라이언트가 공통으로 사용.
"""
import json
import os

# 내장 폴백(파일이 없거나 키가 빠졌을 때). 식별자는 전부 lynnai.
BUILTIN = {
    "brand": "Lynnai",
    "ap_ssid_base": "LYNNAI",
    "setup_ssid_base": "LYNNAI-SETUP",
    "setup_pass": "",
    "default_mode": "standalone",
    "topic_base": "lynnai",         # MQTT 네임스페이스: lynnai/main/*, lynnai/zone/*
    "device_id_prefix": "lynnai",   # HA/ST unique_id: lynnai_<...>
    "manufacturer": "Lynnai",       # 제품 브랜드(실제 보일러 제조사 아님 — 상표 회피)
    "model": "Lynnai Boiler Hub",
    "update_pubkey": "",
}

_PATH = os.environ.get(
    "LYNNAI_DEFAULTS",
    os.path.join(os.path.dirname(__file__), "defaults.json"),
)


def load(path=None):
    """defaults.json 을 BUILTIN 위에 병합. 없으면 BUILTIN. '_'로 시작하는 키는 주석."""
    cfg = dict(BUILTIN)
    p = path or _PATH
    try:
        with open(p) as f:
            data = json.load(f)
        for k, v in data.items():
            if not k.startswith("_"):
                cfg[k] = v
    except FileNotFoundError:
        pass
    return cfg


# ---- 식별자 파생 헬퍼 (모든 디바이스 식별이 여기서 나온다) ----
def topic(cfg, *parts):
    """예: topic(cfg,'main','power') -> 'lynnai/main/power'"""
    return "/".join([cfg["topic_base"], *parts])


def dev_id(cfg, *parts):
    """예: dev_id(cfg,'boiler') -> 'lynnai_boiler', dev_id(cfg,'zone','1') -> 'lynnai_zone_1'"""
    return "_".join([cfg["device_id_prefix"], *parts])


if __name__ == "__main__":
    c = load()
    print("brand        :", c["brand"])
    print("ap_ssid_base :", c["ap_ssid_base"])
    print("topic_base   :", c["topic_base"])
    print("sample topic :", topic(c, "main", "power"))
    print("sample dev_id:", dev_id(c, "zone", "1"))
    print("manufacturer :", c["manufacturer"])
