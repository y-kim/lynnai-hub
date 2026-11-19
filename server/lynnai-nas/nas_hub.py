"""
nas_hub.py -- NAS 허브 조립(ESP32 허브와 동작 동일, LAN 전용). 대응: ESP32 hub_main.c(app_main).
MODE=proxy(기본, 린나이 클라우드로 중계) | standalone(클라우드 없이 허브가 MQTT 상대).

한 프로세스에서 공용 코어(hub_rinnai) 하나를 두고:
  - DNS(:53)        린나이 기기의 rkiot.net 만 NAS 로 (hub_dns=dns_hijack, only_src)
  - MQTT(:8883)     기기 TLS 종단 → proxy: 진짜 클라우드 재연결 / standalone: 허브가 직접 응답(hub_proxy),
                    상태를 코어에 먹임·명령 주입(단독이면 상태 요청·시간 응답도)
  - HTTP(:80)       /key/server 부트스트랩 중계 + /api/v1/state·/metrics (hub_api)
  - LAN 통로(:8484)  SmartThings LAN 드라이버(SSDP + TCP) — core 상태를 내보내고 명령을 코어로 (hub_lan)

빠진 것(ESP32 전용): Wi-Fi AP·Zigbee·Matter·허브에 꽂는 센서 플러그인.
"""
import json
import os
import random
import select
import signal
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
    # 컨테이너 PID 1 이면 SIGTERM 기본 동작이 무시되므로 직접 받아 끝낸다(docker stop 이 10초 기다리지 않게)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    hid = hub_id()
    nas_ip = os.environ.get("NAS_IP", "")
    token = os.environ.get("API_TOKEN", "").strip()
    standalone = os.environ.get("MODE", "proxy") == "standalone"
    log(f"lynnai-nas {'단독' if standalone else '프록시'} 모드 시작  id={hid}  NAS_IP={nas_ip or '(미지정)'}")
    if not os.environ.get("RINNAI_IPS", "").strip():
        log("경고: RINNAI_IPS 가 비어 있음 — 집 LAN 의 아무 기기나 :8883 에 린나이 기기로 붙어 가짜 상태를 올리고 "
            "/key/server 를 받을 수 있음. 보일러·각방 IP 를 고정하고 RINNAI_IPS 에 적을 것")

    core = HubCore(log=lambda *a: log(*a), standalone=standalone)

    # LAN 통로(SmartThings)
    if nas_ip:
        lan = LanUplink(core, hid, nas_ip, DATA, port=int(os.environ.get("LAN_PORT", "8484")), log=lambda *a: log(*a))
        lan.start()
        if os.environ.get("LAN_PAIR", "") not in ("", "0"):   # 새 드라이버(첫 설치 포함)는 이 창에서만 등록
            lan.pairing_open(600)
            log("[lan] LAN_PAIR 켜짐 — 등록이 끝나면 .env 에서 지울 것(켜 두면 재시작마다 600초 열림)")
        elif not lan.tokens and not lan.tok_bad:
            log("[lan] 등록된 드라이버 없음 — LAN_PAIR=1 로 다시 시작해 페어링")
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
