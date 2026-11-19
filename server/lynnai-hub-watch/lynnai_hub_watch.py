"""
lynnai-hub-watch — LYNNAI 허브 장기 기록기 (표준 라이브러리만, SQLite 한 파일).

허브의 서버 인터페이스를 읽어 쌓는다(펌웨어 hub_api.c / hub_events.c):
  GET /api/v1/state   POLL_SEC 마다 → state·zones 표 (보일러·각방·온습도·가스 누적·허브 상태)
  GET /api/events     EVENT_SEC 마다 → events 표 (연결 끊김, Zigbee, 가스 펄스 하나하나)
허브에 API 토큰이 있으면 HUB_TOKEN 으로 보낸다(Authorization: Bearer).

가스 사용량은 펄스 이벤트(정확한 시각)로 센다. 기록기가 잠시 멈춰도 허브 이벤트 버퍼(256개)
안이면 나중에 그대로 채워진다. 그래프는 이 DB 를 Grafana(SQLite 플러그인) 등으로 보거나,
허브 /metrics 를 Prometheus 로 따로 긁어도 된다.

  python lynnai_hub_watch.py                 # 기록 (컨테이너 lynnai-hub-watch)
  python lynnai_hub_watch.py summary [시간]   # 연결 품질 요약 (기본 24)
  python lynnai_hub_watch.py gas [일]         # 날짜별 가스 사용량 (기본 14)
  python lynnai_hub_watch.py temps [시간]     # 방별 온도 시간 평균 (기본 24)

환경변수: HUB_URL(필수, 예 http://192.168.0.50) HUB_TOKEN POLL_SEC(60) EVENT_SEC(30) HUB_DB(data/hub.db)
"""
import json
import os
import sqlite3
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta

HUB_URL = os.environ.get("HUB_URL", "").strip().rstrip("/")
HUB_TOKEN = os.environ.get("HUB_TOKEN", "").strip()
POLL_SEC = int(os.environ.get("POLL_SEC", "60"))
EVENT_SEC = int(os.environ.get("EVENT_SEC", "30"))
DB = os.environ.get("HUB_DB", os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "hub.db"))

# 와이파이 해제 사유(IEEE 802.11 reason) 중 자주 보는 것
REASON = {1: "unspecified", 2: "auth_expire", 3: "기기가 떠남", 4: "무활동", 8: "기기가 떠남(assoc)",
          15: "4way 타임아웃", 200: "비콘 끊김", 201: "AP 못찾음", 202: "인증실패", 204: "핸드셰이크 타임아웃"}
CLOSER = {0: "연결 전 실패", 1: "기기가 닫음", 2: "클라우드가 닫음", 3: "유휴(200초)"}


def db():
    os.makedirs(os.path.dirname(DB), exist_ok=True)
    c = sqlite3.connect(DB)
    c.executescript("""
    create table if not exists events(
      id integer primary key, ts text, boot integer, seq integer, up_ms integer,
      type text, mac text, a integer, b integer, unique(boot, seq));
    create index if not exists ev_ts on events(ts);
    create index if not exists ev_type_ts on events(type, ts);
    create table if not exists polls(
      ts text, ok integer, boot integer, up_ms integer, heap integer, heap_min integer, err text);
    create table if not exists state(
      ts text primary key, up_s integer, heap integer, heap_min integer, rssi integer,
      link_boiler integer, link_rooms integer,
      b_power integer, b_heat integer, b_outing integer, b_hotwater integer, b_preheat integer,
      b_mode text, b_heat_set integer, b_hw_set integer, b_raw text,
      room_t real, room_h real,
      gas_pulses integer, gas_l_total integer, gas_meter_m3 real, gas_flow_lph integer,
      r_raw text);
    create table if not exists zones(
      ts text, zone text, name text, cur integer, setp integer, on_ integer, heating integer,
      primary key(ts, zone));
    """)
    return c


def fetch(path):
    req = urllib.request.Request(HUB_URL + path)
    if HUB_TOKEN:
        req.add_header("Authorization", "Bearer " + HUB_TOKEN)
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.loads(r.read().decode())


def last_seq(c, boot):
    r = c.execute("select max(seq) from events where boot=?", (boot,)).fetchone()
    return -1 if r[0] is None else r[0]


def b(v):
    return None if v is None else int(bool(v))


# ---------------------------------------------------------------- 기록

def poll_state(c):
    now = datetime.now()
    s = fetch("/api/v1/state")
    # 허브가 시간을 맞췄으면 그 시각, 아니면 받은 시각
    ts = (datetime.fromtimestamp(s["time"]) if s.get("time") else now).isoformat(timespec="seconds")
    bo = s.get("boiler") or {}
    rs = s.get("room_sensor") or {}
    g = s.get("gas") or {}
    hub = s.get("hub") or {}
    links = s.get("links") or {}
    c.execute("insert or replace into state values(?,?,?,?,?, ?,?, ?,?,?,?,?, ?,?,?,?, ?,?, ?,?,?,?, ?)", (
        ts, s.get("up_s"), hub.get("heap"), hub.get("heap_min"), hub.get("rssi"),
        b(links.get("boiler")), b(links.get("rooms")),
        b(bo.get("power")), b(bo.get("heat")), b(bo.get("outing")), b(bo.get("hotwater")), b(bo.get("preheat")),
        bo.get("heat_mode"), bo.get("heat_set"), bo.get("hw_set"), bo.get("raw"),
        rs.get("temp"), rs.get("hum"),
        g.get("pulses"), g.get("liters_total"), g.get("meter_m3"), g.get("flow_lph"),
        (s.get("rooms") or {}).get("raw"),
    ))
    for z in (s.get("rooms") or {}).get("zones", []):
        c.execute("insert or replace into zones values(?,?,?,?,?,?,?)",
                  (ts, z["id"], z.get("name"), z.get("current"), z.get("target"), b(z.get("on")), b(z.get("heating"))))
    c.commit()


def poll_events(c):
    now = datetime.now()
    try:
        st = fetch("/api/stats")
    except urllib.error.HTTPError as e:  # 허브는 살아 있고 요청을 거절함(토큰 등) — 응답 없음과 구분
        why = "인증 실패 — HUB_TOKEN 확인" if e.code in (401, 403) else f"오류 응답 {e.code} {e.reason}"
        c.execute("insert into polls values(?,?,?,?,?,?,?)", (now.isoformat(timespec="seconds"), 0, None, None, None, None, f"HTTP {e.code}: {why}"[:120]))
        c.commit()
        print(f"{now:%H:%M:%S} 허브 {why}", flush=True)
        return False
    except Exception as e:  # 허브 꺼짐/와이파이 끊김도 기록 대상
        c.execute("insert into polls values(?,?,?,?,?,?,?)", (now.isoformat(timespec="seconds"), 0, None, None, None, None, str(e)[:120]))
        c.commit()
        print(f"{now:%H:%M:%S} 허브 응답 없음: {e}", flush=True)
        return False
    boot = st["boot"]
    c.execute("insert into polls values(?,?,?,?,?,?,?)",
              (now.isoformat(timespec="seconds"), 1, boot, st["up"], st["heap"], st["heap_min"], None))
    since = last_seq(c, boot) + 1
    got = 0
    while True:
        d = fetch(f"/api/events?since={since}")
        if d["boot"] != boot:          # 그 사이 재부팅 — 다음 폴링에서 새 boot 로
            break
        if d["lost"]:
            print(f"  링 버퍼 넘침: {d['lost']}개 놓침", flush=True)
        for seq, up, typ, mac, a, bb in d["ev"]:
            ts = now - timedelta(milliseconds=d["up"] - up)
            c.execute("insert or ignore into events(ts,boot,seq,up_ms,type,mac,a,b) values(?,?,?,?,?,?,?,?)",
                      (ts.isoformat(timespec="milliseconds"), boot, seq, up, typ, mac, a, bb))
            got += 1
        since = d["next"]
        if len(d["ev"]) < 128:          # 한 번에 최대 128개 — 꽉 찼으면 더 있음
            break
    c.commit()
    if got:
        print(f"{now:%H:%M:%S} 이벤트 {got}개 (boot {boot}, up {st['up'] // 1000}s, 힙 최저 {st['heap_min']})", flush=True)
    return True


def main():
    if not HUB_URL:
        sys.exit("HUB_URL(허브 주소, 예 http://192.168.0.50)을 지정하세요")
    c = db()
    print(f"lynnai-hub-watch 시작: {HUB_URL} 상태 {POLL_SEC}초·이벤트 {EVENT_SEC}초 → {DB}"
          f"{' (토큰 사용)' if HUB_TOKEN else ''}", flush=True)
    next_state = 0.0
    while True:
        try:
            alive = poll_events(c)
            if alive and time.time() >= next_state:
                next_state = time.time() + POLL_SEC
                poll_state(c)
        except urllib.error.HTTPError as e:
            print(f"허브 API 오류 {e.code}: {'토큰 확인(HUB_TOKEN)' if e.code in (401, 403) else e.reason}", flush=True)
        except Exception as e:
            print(f"폴링 오류: {e}", flush=True)
        time.sleep(EVENT_SEC)


# ---------------------------------------------------------------- 보기

def summary(hours):
    c = db()
    since = (datetime.now() - timedelta(hours=hours)).isoformat()
    q = lambda sql, *a: c.execute(sql, (since, *a)).fetchall()
    print(f"== 최근 {hours}시간 (허브 {HUB_URL}) ==")
    p = q("select count(*), sum(ok) from polls where ts>=?")[0]
    if p[0]:
        print(f"허브 응답: {p[1] or 0}/{p[0]} 폴링 성공")
    boots = q("select count(*) from events where ts>=? and type='boot'")[0][0]
    print(f"허브 재부팅: {boots}회")
    print("\n-- 종류별 횟수")
    for t, n in q("select type, count(*) from events where ts>=? group by type order by 2 desc"):
        print(f"  {t:16} {n}")
    print("\n-- AP 해제 (기기별·사유별, 평균 유지)")
    for mac, a, n, avg in q("select mac, a, count(*), avg(b) from events where ts>=? and type='ap_leave' group by mac, a order by 3 desc"):
        print(f"  {mac}  사유 {a:>3} {REASON.get(a, ''):<14} {n:>4}회  평균 {avg / 1000 if avg else 0:,.0f}초 붙어 있었음")
    print("\n-- 클라우드 링크 종료 (기기 IP 끝자리·닫은 쪽, 평균 유지)")
    for dev, who, n, avg in q("""select a, substr(mac,1,2), count(*), avg(b) from events
                                 where ts>=? and type='link_close' group by a, substr(mac,1,2) order by 3 desc"""):
        print(f"  .{dev:<3} {CLOSER.get(int(who, 16), who):<10} {n:>4}회  평균 {avg / 1000 if avg else 0:,.0f}초 유지")
    print("\n-- TLS (기기 쪽 / 클라우드 쪽) 성공 평균·실패")
    for typ in ("link_dev_tls", "link_cloud_tls"):
        ok = q(f"select count(*), avg(b), max(b) from events where ts>=? and type='{typ}' and b>=0")[0]
        bad = q(f"select count(*) from events where ts>=? and type='{typ}' and b<0")[0][0]
        print(f"  {typ:16} 성공 {ok[0]}회 평균 {ok[1] or 0:,.0f}ms 최대 {ok[2] or 0:,}ms / 실패 {bad}회")
    print("\n-- Zigbee")
    for t, n in q("select type, count(*) from events where ts>=? and type like 'zb_%' group by type"):
        print(f"  {t:16} {n}")
    print("\n-- 집 Wi-Fi 끊김 (사유)")
    for a, n in q("select a, count(*) from events where ts>=? and type='sta_down' group by a"):
        print(f"  사유 {a:>3} {REASON.get(a, ''):<14} {n}회")


def gas(days):
    """날짜별 가스 사용량: 펄스 이벤트(b = 1펄스 L)를 날짜로 묶는다."""
    c = db()
    since = (datetime.now() - timedelta(days=days)).strftime("%Y-%m-%d")
    rows = c.execute("""select substr(ts,1,10) d, count(*), sum(b) from events
                        where type='gas_pulse' and ts>=? group by d order by d""", (since,)).fetchall()
    print(f"== 가스 사용량 (최근 {days}일, 펄스 기준) ==")
    total = 0
    for d, n, liters in rows:
        total += liters or 0
        print(f"  {d}  {(liters or 0) / 1000:7.2f} ㎥  ({n}펄스)")
    print(f"  합계      {total / 1000:7.2f} ㎥")
    m = c.execute("select ts, gas_meter_m3, gas_pulses from state where gas_pulses is not null order by ts desc limit 1").fetchone()
    if m:
        print(f"\n마지막 상태 {m[0]}: 누적 {m[2]}펄스" + (f", 계량기 {m[1]:.2f}㎥" if m[1] is not None else " (계량기 숫자 미보정)"))


def temps(hours):
    """방별 온도 시간 평균 + 난방(밸브 열림) 비율."""
    c = db()
    since = (datetime.now() - timedelta(hours=hours)).isoformat()
    print(f"== 방 온도 (최근 {hours}시간, 시간 평균 / 난방 중 비율) ==")
    rows = c.execute("""select substr(ts,1,13) h, name, avg(cur), avg(setp), avg(heating) from zones
                        where ts>=? group by h, zone order by h, zone""", (since,)).fetchall()
    for h, name, cur, setp, heat in rows:
        print(f"  {h}시  {name:4} {cur:5.1f}°C (설정 {setp:4.1f})  난방 {heat * 100:3.0f}%")
    rows = c.execute("select substr(ts,1,13) h, avg(room_t), avg(room_h) from state where ts>=? and room_t is not null group by h order by h",
                     (since,)).fetchall()
    if rows:
        print("\n-- 온습도계")
        for h, t, rh in rows:
            print(f"  {h}시  {t:5.1f}°C  {rh:4.0f}%")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    arg = float(sys.argv[2]) if len(sys.argv) > 2 else None
    if cmd == "summary":
        summary(arg or 24)
    elif cmd == "gas":
        gas(int(arg or 14))
    elif cmd == "temps":
        temps(arg or 24)
    else:
        main()
