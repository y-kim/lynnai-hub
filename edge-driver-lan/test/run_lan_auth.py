#!/usr/bin/env python3
"""LAN 드라이버 인증 경로 시험: 평문 토큰(v1)은 "집 주소"로만 간다 — SSDP 로 다시 찾은 곳에 답한 기기는 토큰을 못 받는다.
  python3 run_lan_auth.py        (lupa 필요: pip install lupa)
"""
import os, sys
from lupa import LuaRuntime
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "src")
H = os.path.join(HERE, "lan_auth.lua")
TOK = "0123456789abcdef0123456789abcdef"
def run(name, scen):
    L = LuaRuntime(unpack_returned_tuples=True)
    L.globals().SCEN = L.table_from({"id": "ABC123", "ssdp": scen.get("ssdp"),
        "peers": L.table_from({k: L.table_from(v) for k, v in scen["peers"].items()}),
        "fields": L.table_from({"token": TOK, **scen["fields"]})})
    sent, home, ip = L.execute(open(H).read(), SRC)
    lines = sent.split("\n") if sent else []
    return lines, home, ip, list(L.globals().LOGS.values())
ok = True
def check(name, cond, lines, home, logs):
    global ok
    print(("PASS " if cond else "FAIL ") + name, "| home=" + home)
    if not cond:
        ok = False; print("\n".join(lines[:20])); print(logs)
# 1) 옛 펌웨어 설치(v2 못 봄), 집 주소 죽음, SSDP 가 옛 판 흉내 기기를 가리킴 → 평문 토큰 금지
for kind in ("legacy", "denied"):
    lines, home, ip, logs = run("imp", {"ssdp": "10.0.0.9:8484", "fields": {"ip": "10.0.0.1", "port": 8484, "home": "10.0.0.1:8484"},
                            "peers": {"10.0.0.9:8484": {"kind": kind, "id": "ABC123"}}})
    check(f"impostor({kind}) gets no plain token", any(l.startswith("10.0.0.9") for l in lines) and not any(l.startswith("10.0.0.9:8484 hello ") for l in lines), lines, home, logs)
# 2) 새로 추가(집 없음): SSDP 로 찾은 옛 펌웨어 허브에 v1 페어링 → 붙고 집 주소 기록
lines, home, ip, logs = run("pair", {"ssdp": "10.0.0.5:8484", "fields": {},
                        "peers": {"10.0.0.5:8484": {"kind": "legacy", "id": "ABC123", "token": TOK}}})
check("first pairing uses v1 and records home", "10.0.0.5:8484 hello " + TOK in lines and home == "10.0.0.5:8484", lines, home, logs)
# 3) 집 주소의 옛 펌웨어 허브 → v1 계속 됨
lines, home, ip, logs = run("home", {"fields": {"ip": "10.0.0.1", "port": 8484, "home": "10.0.0.1:8484"},
                        "peers": {"10.0.0.1:8484": {"kind": "legacy", "id": "ABC123", "token": TOK}}})
check("old firmware at home still gets v1", "10.0.0.1:8484 hello " + TOK in lines, lines, home, logs)
# 4) v2 허브가 옮겨 감: 새 주소에서 v2 로 붙고 집 주소가 새 주소로
lines, home, ip, logs = run("moved", {"ssdp": "10.0.0.7:8484", "fields": {"ip": "10.0.0.1", "port": 8484, "home": "10.0.0.1:8484", "v2": True},
                        "peers": {"10.0.0.7:8484": {"kind": "v2", "id": "ABC123", "token": TOK}}})
check("v2 hub that moved: connects, home follows", home == "10.0.0.7:8484" and not any(" hello " in l for l in lines), lines, home, logs)
# 5) 기존 설치(집 필드 없음, v2 못 봄) 옛 펌웨어 허브가 현 주소에 → 붙고 집 주소 채움
lines, home, ip, logs = run("upgrade", {"fields": {"ip": "10.0.0.1", "port": 8484},
                        "peers": {"10.0.0.1:8484": {"kind": "legacy", "id": "ABC123", "token": TOK}}})
check("existing install fills home", home == "10.0.0.1:8484", lines, home, logs)
sys.exit(0 if ok else 1)
