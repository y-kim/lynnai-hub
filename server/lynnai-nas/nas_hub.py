"""
nas_hub.py -- NAS 허브 조립(ESP32 허브와 동작 동일, LAN 전용). 대응: ESP32 hub_main.c(app_main).
MODE=proxy(기본, 린나이 클라우드로 중계) | standalone(클라우드 없이 허브가 MQTT 상대).

한 프로세스에서 공용 코어(hub_rinnai) 하나를 두고:
  - DNS(:53)        등록된 린나이 기기의 rkiot.net 만 NAS 로, 등록 안 된 출발지가 물으면 후보로 (hub_dns=dns_hijack)
  - MQTT(:8883)     기기 TLS 종단 → proxy: 진짜 클라우드 재연결 / standalone: 허브가 직접 응답(hub_proxy),
                    상태를 코어에 먹임·명령 주입(단독이면 상태 요청·시간 응답도)
  - HTTP(:80)       /key/server 부트스트랩 중계 + /api/v1/state·/metrics + 기기 등록(/devices) (hub_api)
  - LAN 통로(:8484)  SmartThings LAN 드라이버(SSDP + TCP) — core 상태를 내보내고 명령을 코어로 (hub_lan)

등록된 기기 = RINNAI_IPS(고정 IP, 선택) + 승인한 기기(hub_devices: 후보 → /devices 에서 승인, 또는 DEVICE_PAIR=1 의
등록 창). RINNAI_IPS 가 비어 있으면 고정 등록 없이 뜨고, IPv4 가 아닌 항목이 있으면 시작하지 않는다.
모든 출발지를 받으려면 RINNAI_IPS=any 를 명시.

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
import hub_devices
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
    # 가로챌 출발지: 등록된 기기(hub_proxy.is_device — RINNAI_IPS + 승인한 기기)를 질의마다 묻는다. 시작할 때 굳힌
    # 목록이 아니므로 승인하면 다음 질의부터 가로챈다. config 의 only_src(ONLY_SRC)가 있으면 그 목록으로만(좁히기).
    # RINNAI_IPS=any 일 때만 모든 출발지의 질의를 가로챈다(빈 목록 = 제한 없음이 되지 않게).
    only = set(cfg.get("only_src") or [])
    if only:
        log(f"[dns] ONLY_SRC 지정 — {sorted(only)} 의 질의만 가로챔(승인한 기기도 이 목록에 없으면 안 가로챔)")

    def src_ok(ip):
        return ip in only if only else hub_proxy.is_device(ip)

    def unregistered(ip, name):
        """등록 안 된 출발지가 가로채는 이름을 물음 → 후보. 등록 창에서 그 자리에 등록되면 True(이 질의부터 가로챔)."""
        reg = hub_proxy.REGISTRY
        if reg is None or hub_proxy.is_device(ip):       # ONLY_SRC 로 빠진 등록 기기는 후보가 아니다
            return False
        return reg.seen(ip, "dns") and not only

    d = DnsHijack(listen_ip="0.0.0.0", answer_ip=cfg["answer_ip"], hijack_names=cfg.get("hijack", ["rkiot.net"]),
                  logger=lambda *a: log("[dns]", *a),
                  forward=tuple(cfg["forward"]) if cfg.get("forward") else None,
                  catch_all=cfg.get("catch_all", False), is_src_ok=src_ok, on_unregistered=unregistered)
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
    if hub_proxy.DEVICE_ERR:                 # 잘못 적은 값으로는 뜨지 않는다 — 아무 포트도 열기 전에 끝낸다
        log(f"오류: {hub_proxy.DEVICE_ERR} — 시작하지 않음")
        sys.exit(2)
    if hub_proxy.DEVICE_ANY:
        log("경고: RINNAI_IPS=any — 집 LAN 의 아무 기기나 :8883 에 린나이 기기로 붙어 가짜 상태를 올리고 "
            "/key/server 를 받을 수 있음. any 를 지우고 /devices 에서 내 기기만 승인할 것")

    # 기기 등록부(승인한 기기) — DNS·:8883·/key/server 가 hub_proxy.is_device 로 같이 본다
    reg = hub_devices.Devices(DATA, log=lambda *a: log(*a), ignore={nas_ip} if nas_ip else ())
    hub_proxy.REGISTRY = reg
    if not hub_proxy.DEVICE_IP and not hub_proxy.DEVICE_ANY:
        log(f"[dev] RINNAI_IPS 비어 있음 — 고정 등록 기기 없음(승인한 기기 {len(reg.reg)}대만 받음). "
            "후보가 보이면 /devices 에서 승인하거나 DEVICE_PAIR=1 로 다시 시작")
    if os.environ.get("DEVICE_PAIR", "") not in ("", "0"):   # 시작 후 600초 동안 보이는 후보는 바로 등록(ESP32 의 등록 버튼)
        reg.pairing_open(hub_devices.PAIR_SEC)
        log("[dev] DEVICE_PAIR 켜짐 — 등록이 끝나면 .env 에서 지울 것(켜 두면 재시작마다 600초 열림)")

    core = HubCore(log=lambda *a: log(*a), standalone=hub_proxy.FB.local)   # 폴백 중에도 클라우드 노릇을 하게 함수로
    send_mode = lambda: core.emit(0x0B, hub_proxy.mode_payload())           # HUB_RK_CMD_MODE
    hub_proxy.ON_MODE = send_mode                                           # 폴백에 들어가거나 나올 때
    core.on_resend = send_mode                                              # 스마트홈이 붙거나 새로고침할 때
    core.hub_request = hub_proxy.request                                    # "hub fallback=0|1" 등

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
