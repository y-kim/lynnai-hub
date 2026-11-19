#!/usr/bin/env python3
"""
hub_doctor.py -- LYNNAI 허브 문제 진단(PC 에서 실행). 읽기만 한다(허브 상태를 바꾸지 않음).

지금까지 실제로 겪은 장애들을 하나씩 점검하고, 원인 후보와 해결법을 한국어로 알려 준다.

  python3 hub_doctor.py                       # 허브 API 로 점검(기본 192.168.0.50)
  python3 hub_doctor.py --host 192.168.0.50 --token XXXX
  python3 hub_doctor.py --st                  # SmartThings CLI 로 ST 기기 상태도 점검
  python3 hub_doctor.py --serial /dev/ttyUSB0 # USB 시리얼: 로그 수집 + 허브에 'diag' 명령 → 상태 전체 점검
                                              # (집 Wi-Fi 가 끊겨 API 로 못 볼 때 쓰는 길)

LED 오류 코드(빨강 N번 깜빡 → 2초 쉼, 여러 개면 돌아가며):
  1 집 Wi-Fi 연결 안 됨   2 Zigbee 미가입   3 린나이 기기 연결 끊김   4 저장 데이터 오류

주의: 시리얼 포트를 열 때 보드가 리셋되거나 '보일러 온보딩'이 한 번 시작될 수 있다(USB-UART 의 DTR/RTS 가
      리셋·BOOT 버튼에 물려 있음). 그래서 시리얼은 --serial 로 따로 켤 때만 쓴다.
      시리얼 diag 는 비밀값(Wi-Fi·AP 암호, 관리자 암호, API 토큰, 인증서 키)을 출력하지 않는다.

점검 항목(이번에 실제로 있었던 것):
  1. 허브가 집 LAN 에서 안 보임        — Zigbee 재가입 스캔이 라디오를 계속 잡아 Wi-Fi 가 AP 를 못 찾음(사유 201)
  2. Zigbee 미가입 / 가입정보 사라짐   — 부팅 'factory-reset mode', zb_nvs 초기화, 0x03 NO_NETWORK(ST 기기추가 꺼짐)
  3. 저장 데이터 오류(검사값)          — '값 사라짐' / '검사값 불일치', 빨간 LED 깜빡임
  4. 린나이 기기 중계 끊김             — 보일러/온도 조절기가 허브 AP 에 없음, 클라우드 TLS 실패, 부트스트랩 실패
  5. 메모리 부족                       — heap 최저치
  6. 온보딩이 Zigbee 가입을 막음       — 'Zigbee 가입 보류(AP 온보딩 중)' 반복
  7. ST 쪽: LYNNAI Hub/자식 기기 OFFLINE (--st)
"""
import argparse
import json
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

# ---------------------------------------------------------------- 출력

FINDINGS = []          # (등급, 제목, 설명, 해결)
LEVEL = {"OK": "✅", "INFO": "ℹ️ ", "WARN": "⚠️ ", "ERR": "❌"}


LED_CODES = {1: "집 Wi-Fi 연결 안 됨", 2: "Zigbee 미가입(SmartThings 페어링 필요)",
             3: "린나이 기기 연결 끊김", 4: "저장 데이터 오류"}


def led_text(mask):
    return ", ".join(f"{c}번={t}" for c, t in LED_CODES.items() if mask & (1 << (c - 1)))


def add(level, title, detail="", fix=""):
    FINDINGS.append((level, title, detail, fix))


def report():
    order = {"ERR": 0, "WARN": 1, "INFO": 2, "OK": 3}
    print()
    print("=" * 64)
    print(" LYNNAI 허브 진단 결과")
    print("=" * 64)
    for lv, title, detail, fix in sorted(FINDINGS, key=lambda f: order[f[0]]):
        print(f"{LEVEL[lv]} {title}")
        if detail:
            for line in detail.splitlines():
                print(f"     {line}")
        if fix:
            print(f"     → 해결: {fix}")
    errs = sum(1 for f in FINDINGS if f[0] == "ERR")
    warns = sum(1 for f in FINDINGS if f[0] == "WARN")
    print("-" * 64)
    print(f" 오류 {errs} · 주의 {warns}")
    return 1 if errs else 0


# ---------------------------------------------------------------- 허브 API

def get_json(host, path, token, timeout=5):
    req = urllib.request.Request(f"http://{host}{path}")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


STA_REASON = {
    2: "AUTH_EXPIRE", 8: "ASSOC_LEAVE(허브가 Zigbee 가입 동안 Wi-Fi 를 잠깐 멈춤 — 가끔이면 정상)",
    15: "4WAY_HANDSHAKE_TIMEOUT(비밀번호 틀림?)", 201: "NO_AP_FOUND(공유기를 못 찾음)",
    202: "AUTH_FAIL(인증 실패)", 203: "ASSOC_FAIL", 204: "HANDSHAKE_TIMEOUT",
}
ZB_STATUS = {3: "NO_NETWORK — 가입할 열린 망 없음(SmartThings '기기 추가'가 꺼져 있음)"}


def check_reachable(host):
    try:
        urllib.request.urlopen(f"http://{host}/", timeout=5).read(64)
        add("OK", f"허브 응답함 ({host})")
        return True
    except Exception as e:
        add("ERR", f"허브가 집 LAN 에서 안 보임 ({host})", f"{type(e).__name__}: {e}",
            "① 전원/USB 확인 ② --serial 로 로그 확인: '집 Wi-Fi 끊김(사유 201)'이 반복되면 Zigbee 재가입 스캔이 "
            "라디오를 잡는 문제(펌웨어 재시도 간격 확인) ③ 공유기 DHCP 로 IP 가 바뀌었는지 확인(MAC ac:eb:e6:00:00:01 고정 권장)")
        return False


def check_stats(host, token):
    try:
        s = get_json(host, "/api/stats", token)
    except urllib.error.HTTPError as e:
        if e.code in (401, 403):
            add("ERR", "API 토큰 필요", "허브에 API 토큰이 켜져 있음", "--token <토큰> 으로 다시 실행")
        else:
            add("ERR", "/api/stats 실패", str(e))
        return None
    except Exception as e:
        add("ERR", "/api/stats 실패", str(e))
        return None

    c = s.get("count", {})
    up_min = s.get("up", 0) / 60000

    # 2. Zigbee
    if s.get("zb_joined_now"):
        add("OK", "Zigbee 가입됨")
    else:
        detail = f"가동 {up_min:.0f}분 · 가입 실패 {c.get('zb_join_fail', 0)}회 · 성공 {c.get('zb_joined', 0)}회"
        if s.get("zb_factory_new"):
            add("ERR", "Zigbee 가입정보가 없음(초기 상태) — ST 기기가 전부 오프라인", detail,
                "SmartThings 앱 → 기기 추가 → 주변 기기 검색을 켠 뒤, 허브 버튼 짧게 또는 "
                f"http://{host}/api/zigbee/join 호출(3분간 빠른 가입)")
        else:
            add("ERR", "Zigbee 미가입(가입정보는 있음 — 재가입 실패 중)", detail,
                "Station 이 켜져 있는지, 허브와 거리 확인. 계속 실패하면 ST 기기 추가 + 재가입")
    if s.get("zb_nvs_reset"):
        add("ERR", "이번 부팅에 zb_nvs 를 지우고 새로 만듦(Zigbee 가입정보 사라짐)",
            "zb_nvs 파티션 초기화 실패(가득 참/버전 바뀜)", "재페어링 필요. 반복되면 zb_nvs 크기(partitions.csv) 점검")

    # 3. 저장 데이터 검사값
    if s.get("store_bad"):
        add("ERR", f"저장 데이터 오류 {s['store_bad']}건 (빨간 LED 깜빡임)",
            "\n".join(s.get("store_errors", [])),
            "'값 사라짐'은 NVS 가 깨진 항목을 지운 것 — 그 설정을 관리 화면에서 다시 저장. "
            "'검사값 불일치'는 저장 중 전원 끊김 등(한 번 보고 후 자동으로 맞춤). 반복되면 플래시 점검")
    elif "store_bad" in s:
        add("OK", "저장 데이터 검사값 정상")
    lc = s.get("led_codes")
    if lc:
        add("INFO", f"LED 오류 코드: {led_text(lc)}", "빨강 N번 깜빡 → 2초 쉼(여러 개면 돌아가며)",
            "코드 4 는 재부팅 전까지 유지 — 원인 해결 후 재부팅")
    elif lc == 0:
        add("OK", "LED 오류 코드 없음")

    # 5. 메모리
    hm = s.get("heap_min", 0)
    if hm and hm < 30000:
        add("WARN", f"메모리 최저치 낮음 ({hm} B)", "TLS 연결 몰릴 때 실패 가능", "동시 링크 수/버퍼 점검")
    elif hm:
        add("OK", f"메모리 최저치 {hm // 1024} KB")

    # 4. 중계(카운트)
    rej = c.get("link_reject", 0)
    if rej:
        add("WARN", f"중계 연결 거절 {rej}회", "동시 링크 한도 초과 가능", "")
    return s


def check_events(host, token):
    try:
        ev = get_json(host, "/api/events?since=0", token).get("ev", [])
    except Exception as e:
        add("INFO", "이벤트 읽기 실패", str(e))
        return
    # [seq, up_ms, name, mac, a, b]
    fails = [e for e in ev if e[2] == "zb_join_fail"]
    if fails:
        codes = {}
        for e in fails:
            codes[e[4]] = codes.get(e[4], 0) + 1
        desc = ", ".join(f"0x{k:02x}×{v}" + (f" ({ZB_STATUS[k]})" if k in ZB_STATUS else "") for k, v in codes.items())
        gaps = [b[1] - a[1] for a, b in zip(fails, fails[1:])]
        add("WARN", f"Zigbee 가입 실패 {len(fails)}회", desc,
            "0x03 이면 SmartThings 에서 '기기 추가'를 켜야 함")
        if gaps and min(gaps) < 3000:
            add("ERR", "Zigbee 재시도 간격이 너무 짧음(라디오 독점 → Wi-Fi 끊김 원인)",
                f"최소 간격 {min(gaps)} ms", "펌웨어의 재시도 간격(esp_zigbee_alarm_reinit 백오프) 확인")
    downs = [e for e in ev if e[2] == "sta_down"]
    reasons = {}
    for e in downs:
        reasons[e[4]] = reasons.get(e[4], 0) + 1
    bad = {k: v for k, v in reasons.items() if k != 8}
    if bad:
        desc = ", ".join(f"사유 {k} {STA_REASON.get(k, '')} ×{v}" for k, v in bad.items())
        lvl = "ERR" if 201 in bad else "WARN"
        add(lvl, "집 Wi-Fi 끊김", desc,
            "201(AP 못 찾음)이 반복되고 같은 때 Zigbee 가입 시도가 있으면 라디오 경합 — Zigbee 재시도 간격 확인. "
            "15/202 면 Wi-Fi 비밀번호 확인")
    elif reasons:
        add("INFO", f"Wi-Fi 일시 중단 {reasons.get(8, 0)}회(Zigbee 가입 중 정상 동작)")
    tls = [e for e in ev if e[2] == "link_cloud_tls" and e[5] < 0]
    if tls:
        add("WARN", f"클라우드 TLS 실패 {len(tls)}회", f"코드 {sorted(set(e[5] for e in tls))}",
            "인터넷/린나이 서버 상태 확인")
    bs = [e for e in ev if e[2] == "bootstrap" and e[5] == 0]
    if bs:
        add("WARN", f"부트스트랩(/key/server) 실패 {len(bs)}회", "", "린나이 서버(80) 접속 확인")
    ob = [e for e in ev if e[2] == "onboard"]
    if len(ob) > 2:
        add("WARN", f"보일러 온보딩 {len(ob)}회 실행", "온보딩 중엔 Zigbee 가입을 미룸",
            "버튼이 눌린 상태/시리얼 포트 열림 확인")


def check_rinnai(host, token):
    try:
        st = get_json(host, "/api/v1/state", token)
    except Exception as e:
        add("INFO", "/api/v1/state 읽기 실패", str(e))
        return
    boilers, thermos = st.get("boilers", []), st.get("thermostats", [])
    if not boilers and not thermos:
        add("ERR", "린나이 기기 상태가 하나도 없음", "보일러·온도 조절기가 허브 AP 에 안 붙었거나 상태를 아직 못 받음",
            "기기 전원·허브 AP(LYNNAI-HUB-…) 접속 확인, 필요하면 온보딩(버튼 길게 3초)")
        return
    for kind, lst in (("보일러 조절기", boilers), ("온도 조절기", thermos)):
        for d in lst:
            mac = d.get("mac", "?")
            if d.get("linked"):
                add("OK", f"{kind} {mac} 연결됨")
            else:
                add("ERR", f"{kind} {mac} 끊김(마지막 상태만 남음)", "허브 AP 에서 떨어짐",
                    "기기 Wi-Fi/전원 확인 — 허브가 LAN 에서 끊겼었다면 그 영향일 수 있음")


# ---------------------------------------------------------------- SmartThings

def check_st(hub_joined=None):
    cli = shutil.which("smartthings") or "/home/ubuntu/smartthings/smartthings"
    try:
        out = subprocess.run([cli, "devices", "-j"], capture_output=True, text=True, timeout=60).stdout
        devs = json.loads(out)
    except Exception as e:
        add("INFO", "SmartThings CLI 실행 실패", str(e))
        return
    hub = [d for d in devs if (d.get("label") or "").startswith("LYNNAI Hub")]
    if not hub:
        add("ERR", "ST 에 LYNNAI Hub 기기가 없음", "", "ST 기기 추가로 다시 등록")
        return
    hid = hub[0]["deviceId"]
    kids = [d for d in devs if d.get("parentDeviceId") == hid]
    for d in [hub[0]] + kids:
        h = {}
        try:
            h = json.loads(subprocess.run([cli, "devices:health", d["deviceId"], "-j"],
                                          capture_output=True, text=True, timeout=30).stdout)
            state = h.get("state")
        except Exception:
            state = "?"
        when = h.get("lastUpdatedDate", "?")
        if state == "ONLINE" and hub_joined is False:
            # 실측: 허브가 망에서 빠져도 ST 는 한동안 ONLINE 으로 남는다(헬스 갱신이 늦음)
            add("WARN", f"ST {d.get('label')} : ONLINE 으로 보이지만 믿기 어려움",
                f"허브는 Zigbee 미가입인데 ST 표시가 늦게 남아 있음(마지막 갱신 {when})",
                "허브 Zigbee 를 먼저 복구")
        elif state == "ONLINE":
            add("OK", f"ST {d.get('label')} : ONLINE")
        else:
            add("ERR", f"ST {d.get('label')} : {state}", f"마지막 갱신 {when}", "허브 Zigbee 가입·중계 상태를 먼저 확인")
    # 가상 보일러(v<mac>) = 그 온도 조절기에 붙을 실제 보일러가 안 보였다는 뜻(보일러 끊김 신호)
    for d in kids:
        key = (d.get("edgeChild") or {}).get("parentAssignedChildKey", "")
        if key.startswith("v"):
            add("INFO", f"가상 보일러 '{d.get('label')}' 있음 ({key})",
                "그 온도 조절기를 단독으로 판단해 만든 것 — 보통 보일러 조절기가 끊겼을 때 생김",
                "보일러 조절기가 다시 연결되면 자동으로 지워짐(아니면 관리 화면에서 연결 지정)")


# ---------------------------------------------------------------- 시리얼

SERIAL_PATTERNS = [
    ("factory-reset mode", "ERR", "Zigbee 가입정보 없음(초기 상태)", "ST 기기 추가 + 재가입"),
    ("Device Reboot failed", "WARN", "Zigbee 재가입 실패", "Station 상태/거리, 반복이면 재페어링"),
    ("사유 201", "ERR", "집 Wi-Fi 가 AP 를 못 찾음", "Zigbee 스캔과 라디오 경합 여부 확인"),
    ("가입 보류(AP 온보딩 중", "WARN", "온보딩이 Zigbee 가입을 막는 중", "온보딩 끝나길 기다리거나 버튼 확인"),
    ("저장 데이터 오류", "ERR", "저장 데이터 검사값 오류", "/api/stats store_errors 확인"),
    ("zb_nvs 초기화 실패", "ERR", "zb_nvs 를 지우고 새로 만듦", "재페어링, 파티션 크기 점검"),
    ("Guru Meditation", "ERR", "펌웨어 크래시", "백트레이스로 원인 추적"),
    ("abort()", "ERR", "펌웨어 abort", "직전 로그 확인"),
    ("명령 큐 가득", "WARN", "명령 주입 큐 포화", "명령이 너무 몰림"),
]


def open_serial(port):
    import serial  # pyserial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = s.rts = False          # 리셋(EN)·BOOT(GPIO9) 신호를 건드리지 않게(완전히 막지는 못함)
    s.open()
    return s


def check_diag_block(txt):
    """허브의 #DIAG-BEGIN..#DIAG-END 블록을 읽어 점검"""
    kv = {}
    devs = []
    for ln in txt.splitlines():
        if ln.startswith("dev ") and "linked=" in ln:
            p = ln.split()
            devs.append((p[1], p[2], "linked=1" in ln))
        for tok in ln.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                kv.setdefault(k, v)
    import re
    m = re.search(r"reset=(\d+\([^)]*\))", txt)
    if m:
        kv["reset"] = m.group(1)
    add("OK", f"[USB] diag 받음: 펌웨어 {kv.get('fw', '?')} · 가동 {kv.get('up', '?')} · 리셋 사유 {kv.get('reset', '?')}")
    if "패닉" in txt or "워치독" in txt or "전압 부족" in txt:
        add("ERR", "[USB] 직전 재시작이 비정상", f"reset={kv.get('reset')}", "로그에서 Guru Meditation/backtrace 확인")
    lc = int(kv.get("led_codes", "0"), 16)
    if lc:
        add("INFO", f"[USB] LED 오류 코드: {led_text(lc)}")
    if kv.get("provisioned") == "0":
        add("WARN", "[USB] 허브 첫 설정 전", "", "휴대폰으로 LYNNAI-SETUP-… AP 에 접속해 설정")
    if "sta=down" in txt:
        add("ERR" if kv.get("paused") == "0" else "INFO",
            f"[USB] 집 Wi-Fi 끊김 (SSID {kv.get('sta_ssid', '?')}, {kv.get('down_for', '?')})",
            "paused=1 이면 Zigbee 가입 중 잠시 멈춘 것(정상)",
            "공유기 켜짐·SSID/암호 변경 여부 확인. 암호가 바뀌었으면 허브 공장초기화 후 재설정")
    elif "sta=up" in txt:
        add("OK", f"[USB] 집 Wi-Fi 연결 (IP {kv.get('ip')}, 신호 {kv.get('rssi')} dBm)")
        if int(kv.get("rssi", "0")) < -80:
            add("WARN", "[USB] 집 Wi-Fi 신호 약함", f"{kv.get('rssi')} dBm", "허브를 공유기 가까이")
    if kv.get("joined") == "1":
        add("OK", f"[USB] Zigbee 가입 (PAN {kv.get('pan')}, 채널 {kv.get('ch')})")
    elif "joined" in kv:
        add("ERR", "[USB] Zigbee 미가입" + (" — 가입정보 없음(초기 상태)" if kv.get("factory_new") == "1" else ""),
            f"nvs_reset={kv.get('nvs_reset')}", "SmartThings 기기 추가 켜고 허브 버튼 짧게")
    for kind, mac, ok in devs:
        add("OK" if ok else "WARN", f"[USB] {kind} {mac} {'연결됨' if ok else '끊김'}", "",
            "" if ok else "기기 전원·허브 AP 접속(ap_clients) 확인, 필요하면 허브 버튼 길게 3초(온보딩)")
    sb = int(kv.get("store_bad", "0"))
    if sb:
        add("ERR", f"[USB] 저장 데이터 오류 {sb}건", kv.get("errors", ""), "해당 설정을 관리 화면에서 다시 저장 후 재부팅")


def check_serial(port, seconds):
    try:
        import serial  # noqa: F401  pyserial
    except ImportError:
        add("INFO", "pyserial 없음", "", "pip install pyserial")
        return
    print(f"시리얼 {port}: 로그 {seconds}초 수집 후 diag 요청…")
    buf = b""
    try:
        s = open_serial(port)
        end = time.time() + seconds
        while time.time() < end:
            buf += s.read(4096)
        s.write(b"diag\n")
        end = time.time() + 8
        while time.time() < end and b"#DIAG-END" not in buf:
            buf += s.read(4096)
        s.close()
    except Exception as e:
        add("WARN", "시리얼 열기 실패", str(e), "다른 프로그램이 포트를 쓰는지 확인(WSL 이면 usbipd attach)")
        return
    txt = buf.decode("utf-8", "replace")
    lines = [ln.strip() for ln in txt.splitlines() if ln.strip()]
    i, j = txt.find("#DIAG-BEGIN"), txt.find("#DIAG-END")
    if i >= 0 and j > i:
        block = txt[i:j + 9]
        name = time.strftime("hub_diag_%Y%m%d_%H%M%S.txt")
        with open(name, "w", encoding="utf-8") as f:
            f.write(block + "\n")
        add("INFO", f"[USB] diag 저장: {name}", "비밀값 없음 — 그대로 공유해도 됨")
        check_diag_block(block)
    else:
        add("WARN", "[USB] diag 응답 없음", "펌웨어가 오래됐거나 부팅 중",
            "보드의 'UART' 포트(CP210x)에 연결했는지 확인. 'USB' 포트는 로그만 나옴")
    hit = False
    for pat, lv, title, fix in SERIAL_PATTERNS:
        m = [ln for ln in lines if pat in ln]
        if m:
            hit = True
            add(lv, f"[시리얼] {title} ({len(m)}회)", m[-1][:150], fix)
    if not hit:
        add("OK", f"[시리얼] 알려진 오류 패턴 없음 ({len(lines)}줄)")


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description="LYNNAI 허브 진단(읽기 전용)")
    ap.add_argument("--host", default="192.168.0.50")
    ap.add_argument("--token", default="")
    ap.add_argument("--st", action="store_true", help="SmartThings CLI 로 ST 기기 상태도 점검")
    ap.add_argument("--serial", default="", help="시리얼 포트(예: /dev/ttyUSB0) — 켜면 로그도 점검")
    ap.add_argument("--seconds", type=int, default=10, help="diag 전 시리얼 로그 수집 시간")
    a = ap.parse_args()

    joined = None
    if check_reachable(a.host):
        st = check_stats(a.host, a.token)
        joined = st.get("zb_joined_now") if st else None
        check_events(a.host, a.token)
        check_rinnai(a.host, a.token)
    if a.st:
        check_st(joined)
    if a.serial:
        check_serial(a.serial, a.seconds)
    sys.exit(report())


if __name__ == "__main__":
    main()
