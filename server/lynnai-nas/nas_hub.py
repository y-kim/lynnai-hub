"""
nas_hub.py -- NAS proxy 모드 조립(ESP32 허브와 동작 동일, LAN 전용). 대응: ESP32 hub_main.c(app_main).

한 프로세스에서 공용 코어(hub_rinnai) 하나를 두고:
  - DNS(:53)        린나이 기기의 rkiot.net 만 NAS 로 (hub_dns=dns_hijack, only_src)
  - MQTT 중계(:8883) 기기 TLS 종단 → 진짜 클라우드 재연결(hub_proxy), 지나가는 상태를 코어에 먹임·명령 주입
  - HTTP(:80)       /key/server 부트스트랩 중계 + /api/v1/state·/metrics (hub_api)
  - LAN 통로(:8484)  SmartThings LAN 드라이버(SSDP + TCP) — core 상태를 내보내고 명령을 코어로 (hub_lan)

빠진 것(ESP32 전용): Wi-Fi AP·Zigbee·Matter·허브에 꽂는 센서 플러그인.
"""
import json
import os
import random
import select
import string
import sys
import threading

import hub_proxy
from hub_rinnai import HubCore
from hub_lan import LanUplink
import hub_api
from dns_hijack import DnsHijack

DATA = os.environ.get("DATA_DIR", "/data")


def log(*a):
    import datetime
    print(datetime.datetime.now().isoformat(timespec="seconds"), *a, flush=True)


def hub_id():
    p = os.path.join(DATA, "id")
    try:
        return open(p).read().strip()
    except OSError:
        i = "".join(random.choice(string.ascii_uppercase + string.digits) for _ in range(6))
        open(p, "w").write(i)
        return i


def start_dns(core_log):
    try:
        cfg = json.load(open(os.path.join(DATA, "config.json")))["dns"]
    except Exception as e:
        log("[dns] config 없음, DNS 생략:", e)
        return
    if not cfg.get("enable", True):
        return
    d = DnsHijack(listen_ip="0.0.0.0", answer_ip=cfg["answer_ip"], hijack_names=cfg.get("hijack", ["rkiot.net"]),
                  logger=lambda *a: log("[dns]", *a),
                  forward=tuple(cfg["forward"]) if cfg.get("forward") else None,
                  catch_all=cfg.get("catch_all", False), only_src=cfg.get("only_src"))
    d.open(cfg.get("port", 53))

    def loop():
        while True:
            r, _, _ = select.select([d.sock], [], [], 1.0)
            if r:
                try: d.handle_once()
                except Exception as e: log("[dns] err", e)
    threading.Thread(target=loop, daemon=True).start()


def main():
    hid = hub_id()
    nas_ip = os.environ.get("NAS_IP", "")
    token = os.environ.get("API_TOKEN", "").strip()
    log(f"lynnai-nas proxy 모드 시작  id={hid}  NAS_IP={nas_ip or '(미지정)'}")

    core = HubCore(log=lambda *a: log(*a))

    # LAN 통로(SmartThings)
    if nas_ip:
        lan = LanUplink(core, hid, nas_ip, DATA, port=int(os.environ.get("LAN_PORT", "8484")), log=lambda *a: log(*a))
        lan.start()
        if os.environ.get("LAN_PAIR"):        # 처음 설치 외에 새 드라이버를 붙일 때
            lan.pairing_open(600)
    else:
        log("[lan] NAS_IP 가 없어 LAN 통로를 끕니다(SmartThings 연동 불가)")

    # 서버 인터페이스 + /key/server (:80)
    hub_api.serve(core, hid, int(os.environ.get("HTTP_PORT", "80")), token, log=lambda *a: log(*a))

    # DNS
    start_dns(log)

    # MQTT 중계(:8883) — 블로킹
    hub_proxy.run(core)


if __name__ == "__main__":
    main()
