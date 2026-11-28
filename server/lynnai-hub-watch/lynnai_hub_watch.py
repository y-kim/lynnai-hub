"""
lynnai-hub-watch — LYNNAI 허브 장기 기록기 (표준 라이브러리만, SQLite 한 파일).

허브의 서버 인터페이스를 읽어 쌓는다(펌웨어 hub_api.c / hub_events.c):
  GET /api/v1/state   POLL_SEC 마다 → state·zones 표 (보일러·각방·온습도·가스 누적·허브 상태)
                      zones 는 조절기(mac·층 라벨)별 — 위성 허브가 올려보낸 다른 층 방도 같이
  GET /api/events     EVENT_SEC 마다 → events 표 (연결 끊김, Zigbee, 가스 펄스 하나하나)
허브에 API 토큰이 있으면 HUB_TOKEN 으로 보낸다(Authorization: Bearer).

가스 사용량은 펄스 이벤트(정확한 시각)로 센다. 기록기가 잠시 멈춰도 허브 이벤트 버퍼(256개)
안이면 나중에 그대로 채워진다. 그래프는 이 DB 를 Grafana(SQLite 플러그인) 등으로 보거나,
허브 /metrics 를 Prometheus 로 따로 긁어도 된다.
  python lynnai_hub_watch.py burn [시간]  — 가스↔연소(난방/온수) 상관, 각방 외출 전환 시각

  python lynnai_hub_watch.py                 # 기록 (컨테이너 lynnai-hub-watch)
  python lynnai_hub_watch.py summary [시간]   # 연결 품질 요약 (기본 24)
  python lynnai_hub_watch.py gas [일]         # 날짜별 가스 사용량 (기본 14)
  python lynnai_hub_watch.py temps [시간]     # 방별 온도 시간 평균 (기본 24)
  python lynnai_hub_watch.py usage [일]       # 보일러 자체 사용량 누계(stg, 1시간마다) — 날짜별·시간별 (기본 7)
  python lynnai_hub_watch.py import-stg 파일.jsonl ...   # 지난 허브 통신 기록(hubtrace)의 보일러 stg 를 boiler_usage 로

보일러 사용량(boiler_usage 표): 보일러가 약 1시간마다 올리는 누계(stg)를 허브가 받아 boil_stg 이벤트로 낸다
(프록시·단독 모두 — 단독이면 클라우드가 없어 이게 유일한 원천). 값은 정수(㎥, 시간)이고 그 시간 사용량은 앞 줄과의 차.

환경변수: HUB_URL(필수, 예 http://192.168.0.50) HUB_TOKEN POLL_SEC(60) EVENT_SEC(30) HUB_DB(data/hub.db)
  HUB_DB_URL  주면 SQLite 파일 대신 PostgreSQL 에 쓴다 (예 postgresql://user:암호@192.168.0.7:5432/lynnai_hub,
              psycopg 3 필요: pip install "psycopg[binary]"). 비우면 HUB_DB 파일.
  WEB_PORT    주면 그 포트에 로그 브라우저(읽기 전용 웹 화면 lynnai_hub_web.html + /api/log/*)를 같이 띄운다.
  python lynnai_hub_watch.py web [포트]       # 기록 없이 로그 브라우저만 (기본 포트 WEB_PORT 또는 8497)
  python lynnai_hub_watch.py to-pg [hub.db]   # SQLite 기록을 HUB_DB_URL 로 옮긴다(이미 있는 줄은 건너뜀 — 여러 번 돌려도 됨)
시각(ts)은 두 DB 모두 ISO 글자('2026-11-27T13:00:00', 이벤트는 밀리초까지). Postgres 에선 ts::timestamp 로 바꿔 쓰면 된다.
"""
import json
import os
import re
import sqlite3
import sys
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timedelta

HERE = os.path.dirname(os.path.abspath(__file__))
HUB_URL = os.environ.get("HUB_URL", "").strip().rstrip("/")
HUB_TOKEN = os.environ.get("HUB_TOKEN", "").strip()
POLL_SEC = int(os.environ.get("POLL_SEC", "60"))
EVENT_SEC = int(os.environ.get("EVENT_SEC", "30"))
DB = os.environ.get("HUB_DB", os.path.join(HERE, "data", "hub.db"))
DB_URL = os.environ.get("HUB_DB_URL", "").strip()
WEB_PORT = int(os.environ.get("WEB_PORT", "0") or 0)   # 로그 브라우저(읽기 전용). 0 이면 안 띄움
WEB_HTML = os.path.join(os.path.dirname(os.path.abspath(__file__)), "lynnai_hub_web.html")
# 로그 브라우저 로그인 정보(아이디·암호 해시·세션 서명 키). 없으면 처음 들어온 사람이 만든다.
# 잊으면 이 파일을 지우면 된다 → 다음 접속에서 다시 만들기 화면
WEB_AUTH = os.environ.get("HUB_WEB_AUTH", os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "hubweb-auth.json"))

# 와이파이 해제 사유(IEEE 802.11 reason) 중 자주 보는 것
REASON = {1: "unspecified", 2: "auth_expire", 3: "기기가 떠남", 4: "무활동", 8: "기기가 떠남(assoc)",
          15: "4way 타임아웃", 200: "비콘 끊김", 201: "AP 못찾음", 202: "인증실패", 204: "핸드셰이크 타임아웃"}
CLOSER = {0: "연결 전 실패", 1: "기기가 닫음", 2: "클라우드가 닫음", 3: "유휴(200초)"}


class Store:
    """SQLite 파일(HUB_DB) 또는 PostgreSQL(HUB_DB_URL). 부르는 쪽 SQL 은 SQLite 문법(? 자리표시)이고
    Postgres 면 여기서 %s 로 바꾼다. 'insert or replace/ignore' 는 insert() 로 — Postgres 는 on conflict."""

    def __init__(self, url=None):
        self.url = url
        self.pg = bool(url)
        self.open()

    def open(self):
        if self.pg:
            import psycopg
            from psycopg.types.numeric import FloatLoader
            self.c = psycopg.connect(self.url, connect_timeout=10)
            self.c.adapters.register_loader("numeric", FloatLoader)   # avg()·나눗셈이 Decimal 대신 float
        else:
            os.makedirs(os.path.dirname(DB) or ".", exist_ok=True)
            self.c = sqlite3.connect(DB)

    def where(self):
        return re.sub(r"//[^@/]*@", "//", self.url) if self.pg else DB     # 암호는 빼고 보여 준다

    def execute(self, sql, params=()):
        if not self.pg:
            return self.c.execute(sql, params)
        if params:
            return self.c.execute(sql.replace("%", "%%").replace("?", "%s"), params)
        return self.c.execute(sql)

    def insert(self, table, cols, row, key=None, replace=False):
        """한 줄 넣기. 같은 키(유일 제약)가 있으면 replace 면 덮어쓰고 아니면 건너뛴다."""
        names, marks = ",".join(cols), ",".join("?" * len(cols))
        if not self.pg:
            return self.c.execute(f"insert or {'replace' if replace else 'ignore'} into {table}({names}) values({marks})", row)
        tail = " on conflict do nothing"
        if replace:
            tail = f" on conflict({','.join(key)}) do update set " + ",".join(f"{x}=excluded.{x}" for x in cols if x not in key)
        return self.execute(f"insert into {table}({names}) values({marks}){tail}", row)

    def commit(self):
        self.c.commit()

    def close(self):
        try:
            self.c.close()
        except Exception:
            pass

    def recover(self):
        """오류 뒤: 하다 만 트랜잭션을 되돌린다(Postgres 는 안 되돌리면 다음 문장이 다 실패). 연결이 끊겼으면 다시 붙는다."""
        try:
            self.c.rollback()
        except Exception:
            self.close()
            try:
                self.open()
            except Exception as e:
                print(f"DB 다시 연결 실패: {e}", flush=True)


EV_COLS = ("ts", "boot", "seq", "up_ms", "type", "mac", "a", "b")
POLL_COLS = ("ts", "ok", "boot", "up_ms", "heap", "heap_min", "err")
STATE_COLS = ("ts", "up_s", "heap", "heap_min", "rssi", "link_boiler", "link_rooms",
              "b_power", "b_heat", "b_outing", "b_hotwater", "b_preheat", "b_mode", "b_heat_set", "b_hw_set", "b_raw",
              "room_t", "room_h", "gas_pulses", "gas_l_total", "gas_meter_m3", "gas_flow_lph", "r_raw")
ZONE_COLS = ("ts", "zone", "name", "cur", "setp", "on_", "heating", "mac", "label", "outing", "reserve", "ivl")
USAGE_COLS = ("ts", "src", "mac", "heat_gas_m3", "hw_gas_m3", "heat_burn_h", "hw_burn_h")
TABLES = {"events": EV_COLS, "polls": POLL_COLS, "state": STATE_COLS, "zones": ZONE_COLS, "boiler_usage": USAGE_COLS}


def schema(pg):
    """두 DB 의 표. Postgres: 정수는 bigint(up_ms 등이 2^31 을 넘는다), ts 는 C 정렬(ISO 글자를 바이트 순으로 비교)."""
    I, R = ("bigint", "double precision") if pg else ("integer", "real")
    T = 'text collate "C"' if pg else "text"
    ident = "id bigserial primary key" if pg else "id integer primary key"
    return [
        f"""create table if not exists events({ident}, ts {T}, boot {I}, seq {I}, up_ms {I},
            type text, mac text, a {I}, b {I}, unique(boot, seq))""",
        "create index if not exists ev_ts on events(ts)",
        "create index if not exists ev_type_ts on events(type, ts)",
        # Postgres 는 옮기기를 다시 돌려도 겹치지 않게 ts 유일(30초마다 한 줄이라 겹칠 일 없음)
        f"""create table if not exists polls(ts {T}, ok {I}, boot {I}, up_ms {I}, heap {I}, heap_min {I}, err text
            {', unique(ts)' if pg else ''})""",
        f"""create table if not exists state(
            ts {T} primary key, up_s {I}, heap {I}, heap_min {I}, rssi {I},
            link_boiler {I}, link_rooms {I},
            b_power {I}, b_heat {I}, b_outing {I}, b_hotwater {I}, b_preheat {I},
            b_mode text, b_heat_set {I}, b_hw_set {I}, b_raw text,
            room_t {R}, room_h {R},
            gas_pulses {I}, gas_l_total {I}, gas_meter_m3 {R}, gas_flow_lph {I},
            r_raw text)""",
        f"""create table if not exists boiler_usage(
            ts {T} primary key, src text, mac text,
            heat_gas_m3 {I}, hw_gas_m3 {I}, heat_burn_h {I}, hw_burn_h {I})""",
    ]


def zones_sql(pg):
    # Postgres 의 primary key 는 NULL 을 못 받는다(옛 펌웨어는 mac 이 없다) → unique 로
    I, T = ("bigint", 'text collate "C"') if pg else ("integer", "text")
    return f"""create table if not exists zones(
      ts {T}, zone text, name text, cur {I}, setp {I}, on_ {I}, heating {I},
      mac text, label text, outing {I}, reserve {I}, ivl {I},
      {'unique' if pg else 'primary key'}(ts, mac, zone))"""


def db(url=None):
    c = Store(DB_URL if url is None else url)
    for sql in schema(c.pg):
        c.execute(sql)
    if c.pg:
        c.execute(zones_sql(True))
    else:
        migrate_zones(c)
    c.execute("create index if not exists zones_ts on zones(ts)")
    c.commit()
    return c


def migrate_zones(c):
    """(SQLite) zones 표: 조절기(mac)별로 구분한다(멀티 허브 — 1층·2층 조절기가 둘 다 z1..z7 을 쓴다).
    옛 표(mac 칸 없음)는 새 표로 옮기고 mac 은 비워 둔다 — 옛 줄은 모두 로컬 조절기 것이라
    poll_state 가 허브에서 로컬 조절기 mac 을 알게 되면 채운다."""
    cols = [r[1] for r in c.execute("pragma table_info(zones)")]
    if cols and "mac" not in cols:
        c.execute("begin")
        c.execute("alter table zones rename to zones_old")
        c.execute(zones_sql(False))
        c.execute("insert into zones(ts, zone, name, cur, setp, on_, heating) "
                  "select ts, zone, name, cur, setp, on_, heating from zones_old")
        c.execute("drop table zones_old")
        c.execute("commit")
        print("zones 표를 조절기(mac)별로 옮겼습니다", flush=True)
    else:
        c.execute(zones_sql(False))


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


def add_usage(c, ts, src, mac, hg, wg, hh, wh):
    """보일러 stg 누계 한 줄. 같은 stg 를 두 원천(허브 이벤트·지난 통신 기록)에서 받을 수 있어 앞뒤 2분 안에 있으면 건너뛴다."""
    t = datetime.fromisoformat(ts)
    lo, hi = (t - timedelta(minutes=2)).isoformat(timespec="seconds"), (t + timedelta(minutes=2)).isoformat(timespec="seconds")
    if c.execute("select 1 from boiler_usage where ts between ? and ?", (lo, hi)).fetchone():
        return False
    c.insert("boiler_usage", USAGE_COLS, (t.isoformat(timespec="seconds"), src, mac, hg, wg, hh, wh))
    return True


def stg_event(c, ts, mac, a, bb):
    """boil_stg 이벤트: a = 난방 가스 | 온수 가스 << 16, b = 난방 시간 | 온수 시간 << 16"""
    return add_usage(c, ts, "hub", mac, a & 0xffff, (a >> 16) & 0xffff, bb & 0xffff, (bb >> 16) & 0xffff)


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
    c.insert("state", STATE_COLS, (
        ts, s.get("up_s"), hub.get("heap"), hub.get("heap_min"), hub.get("rssi"),
        b(links.get("boiler")), b(links.get("rooms")),
        b(bo.get("power")), b(bo.get("heat")), b(bo.get("outing")), b(bo.get("hotwater")), b(bo.get("preheat")),
        bo.get("heat_mode"), bo.get("heat_set"), bo.get("hw_set"), bo.get("raw"),
        rs.get("temp"), rs.get("hum"),
        g.get("pulses"), g.get("liters_total"), g.get("meter_m3"), g.get("flow_lph"),
        (s.get("rooms") or {}).get("raw"),
    ), key=("ts",), replace=True)
    # 각방: thermostats[] = 조절기 전부(로컬 + 위성 허브가 올려보낸 원격). 옛 펌웨어는 rooms(첫 조절기)만.
    therms = s.get("thermostats")
    if therms is None:
        therms = [{"mac": None, "label": None, "zones": (s.get("rooms") or {}).get("zones", [])}]
    global _old_rows_tagged
    for t in therms:
        mac = t.get("mac")
        if mac and not t.get("remote") and not _old_rows_tagged:
            c.execute("update zones set mac=?, label=? where mac is null", (mac, t.get("label")))
            _old_rows_tagged = True
        for z in t.get("zones") or []:
            c.insert("zones", ZONE_COLS,
                     (ts, z["id"], z.get("name"), z.get("current"), z.get("target"), b(z.get("on")), b(z.get("heating")),
                      mac, t.get("label"), b(z.get("outing")), b(z.get("reserve")), z.get("interval")),
                     key=("ts", "mac", "zone"), replace=True)
    c.commit()


_old_rows_tagged = False   # 옛 줄(mac 없음)에 로컬 조절기 mac 을 한 번 채웠는지


def poll_events(c):
    now = datetime.now()
    try:
        st = fetch("/api/stats")
    except urllib.error.HTTPError as e:  # 허브는 살아 있고 요청을 거절함(토큰 등) — 응답 없음과 구분
        why = "인증 실패 — HUB_TOKEN 확인" if e.code in (401, 403) else f"오류 응답 {e.code} {e.reason}"
        c.insert("polls", POLL_COLS, (now.isoformat(timespec="seconds"), 0, None, None, None, None, f"HTTP {e.code}: {why}"[:120]))
        c.commit()
        print(f"{now:%H:%M:%S} 허브 {why}", flush=True)
        return False
    except Exception as e:  # 허브 꺼짐/와이파이 끊김도 기록 대상
        c.insert("polls", POLL_COLS, (now.isoformat(timespec="seconds"), 0, None, None, None, None, str(e)[:120]))
        c.commit()
        print(f"{now:%H:%M:%S} 허브 응답 없음: {e}", flush=True)
        return False
    boot = st["boot"]
    c.insert("polls", POLL_COLS, (now.isoformat(timespec="seconds"), 1, boot, st["up"], st["heap"], st["heap_min"], None))
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
            c.insert("events", EV_COLS, (ts.isoformat(timespec="milliseconds"), boot, seq, up, typ, mac, a, bb))
            if typ == "boil_stg":
                stg_event(c, ts.isoformat(timespec="seconds"), mac, a, bb)
            got += 1
        since = d["next"]
        if len(d["ev"]) < 128:          # 한 번에 최대 128개 — 꽉 찼으면 더 있음
            break
    c.commit()
    if got:
        print(f"{now:%H:%M:%S} 이벤트 {got}개 (boot {boot}, up {st['up'] // 1000}s, 힙 최저 {st['heap_min']})", flush=True)
    return True


def catch_up_usage(c):
    """events 에 이미 있는 boil_stg 를 boiler_usage 로(이 기능 없던 코드가 받아 둔 것 포함). 겹치면 add_usage 가 건너뛴다."""
    n = 0
    for ts, mac, a, bb in c.execute("select ts, mac, a, b from events where type='boil_stg' order by ts").fetchall():
        n += stg_event(c, ts[:19], mac, a, bb)
    c.commit()
    if n:
        print(f"보일러 사용량: 이벤트에서 {n}줄 옮김", flush=True)


def main():
    if not HUB_URL:
        sys.exit("HUB_URL(허브 주소, 예 http://192.168.0.50)을 지정하세요")
    me = os.path.abspath(__file__)
    stamp = os.stat(me).st_mtime_ns
    c = db()
    catch_up_usage(c)
    if WEB_PORT:
        start_web(WEB_PORT)
    print(f"lynnai-hub-watch 시작: {HUB_URL} 상태 {POLL_SEC}초·이벤트 {EVENT_SEC}초 → {c.where()}"
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
            c.recover()
        except Exception as e:
            print(f"폴링 오류: {e}", flush=True)
            c.recover()
        time.sleep(EVENT_SEC)
        try:
            if os.stat(me).st_mtime_ns != stamp:   # 배포로 이 파일이 바뀌면 새 코드로 다시 뜬다(컨테이너 재시작 불필요)
                print("코드가 바뀌어 다시 뜹니다", flush=True)
                c.close()
                os.execv(sys.executable, [sys.executable, "-u", me])
        except OSError:
            pass


# ---------------------------------------------------------------- 보기

def burn(hours):
    """가스↔연소 상관: boil_burn 이벤트로 모드별 구간을 만들고, 그 구간의 가스 사용(가스 누적펄스 델타)을 합산.
    가스 하나하나는 gas_pulse 이벤트로도 있으니 시각으로 더 세밀히 맞출 수도 있다."""
    c = db()
    since = (datetime.now() - timedelta(hours=hours)).isoformat()
    rows = c.execute("select ts, a, b from events where type='boil_burn' and ts>=? order by ts", (since,)).fetchall()
    if len(rows) < 2:
        print("boil_burn 이벤트가 부족합니다 (새 펌웨어 필요하거나 수집을 더 기다리세요)."); return
    step = c.execute("select gas_l_total*1.0/gas_pulses from state where gas_pulses>0 order by ts desc limit 1").fetchone()
    L = (step and step[0]) or 10.0                      # 1펄스 L
    MODE = {0: "대기(연소 없음)", 1: "난방 연소", 2: "온수 연소", 3: "난방+온수"}
    agg = {}                                             # mode -> [sec, pulses]
    for (t1, a1, b1), (t2, a2, b2) in zip(rows, rows[1:]):
        sec = (datetime.fromisoformat(t2) - datetime.fromisoformat(t1)).total_seconds()
        dp = (b2 - b1) if (b2 is not None and b1 is not None and b2 >= b1) else 0
        A = agg.setdefault(a1, [0.0, 0]); A[0] += sec; A[1] += dp
    print(f"최근 {hours:g}시간 · 1펄스 {L:g} L · boil_burn 이벤트 {len(rows)}개")
    print(f"{'모드':14} {'시간(분)':>9} {'가스(L)':>9} {'L/시간':>8}")
    for m in sorted(agg):
        sec, p = agg[m]; liters = p * L
        print(f"{MODE.get(m, str(m)):14} {sec/60:9.1f} {liters:9.1f} {(liters/(sec/3600) if sec else 0):8.1f}")
    heat = agg.get(1, [0, 0])[1] * L + agg.get(3, [0, 0])[1] * L
    hw = agg.get(2, [0, 0])[1] * L
    if heat + hw > 0:
        print(f"\n가스 배분: 난방 {heat:.0f} L ({100*heat/(heat+hw):.0f}%) · 온수 {hw:.0f} L ({100*hw/(heat+hw):.0f}%)")
    outs = c.execute("select ts, mac, a, b from events where type='room_out' and ts>=? order by ts", (since,)).fetchall()
    if outs:
        print("\n각방 외출 전환(최근 20):")
        for ts, mac, a, b in outs[-20:]:
            print(f"  {ts[:19]} 존{a} {'외출 켬' if b else '외출 끔'} (f09 {mac})")


def summary(hours):
    c = db()
    since = (datetime.now() - timedelta(hours=hours)).isoformat()
    q = lambda sql, *a: c.execute(sql, (since, *a)).fetchall()
    print(f"== 최근 {hours}시간 (허브 {HUB_URL}) ==")
    p = q("select count(*), sum(ok) from polls where ts>=?")[0]
    if p[0]:
        print(f"허브 응답: {int(p[1] or 0)}/{p[0]} 폴링 성공")
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


def usage(days):
    """보일러 자체 사용량(stg 누계)의 날짜별·시간별 차."""
    c = db()
    since = (datetime.now() - timedelta(days=days)).isoformat(timespec="seconds")
    rows = c.execute("select ts, heat_gas_m3, hw_gas_m3, heat_burn_h, hw_burn_h, src from boiler_usage order by ts").fetchall()
    if not rows:
        print("boiler_usage 가 비어 있습니다")
        return
    print(f"== 보일러 사용량 (stg 누계, 최근 {days}일) — 기록 {len(rows)}줄 {rows[0][0]} ~ {rows[-1][0]} ==")
    day = {}
    prev = None
    hours = []
    for r in rows:
        if prev and r[0] >= since:
            d = [r[i] - prev[i] for i in range(1, 5)]
            if min(d) < 0:            # 누계가 줄었으면(보일러 쪽 초기화) 그 칸은 건너뜀
                d = [0, 0, 0, 0]
            k = r[0][:10]
            day[k] = [x + y for x, y in zip(day.get(k, [0, 0, 0, 0]), d)]
            gap = (datetime.fromisoformat(r[0]) - datetime.fromisoformat(prev[0])).total_seconds() / 3600
            hours.append((r[0], d, gap))
        prev = r
    print("  날짜        난방가스 온수가스  난방h 온수h")
    for k, (hg, wg, hh, wh) in sorted(day.items()):
        print(f"  {k}  {hg:6} ㎥ {wg:5} ㎥ {hh:5} {wh:5}")
    print("\n-- 쓴 시간만 (그 줄까지의 차, 앞 줄과 1.5시간 넘게 떨어졌으면 표시)")
    for ts, (hg, wg, hh, wh), gap in hours:
        if hg or wg or hh or wh:
            print(f"  {ts[5:16]}  난방 +{hg} ㎥ 온수 +{wg} ㎥  버너 +{hh}h/+{wh}h" + (f"  ({gap:.1f}시간치)" if gap > 1.5 else ""))
    last = rows[-1]
    print(f"\n마지막 누계 {last[0]} ({last[5]}): 난방 가스 {last[1]} ㎥, 온수 가스 {last[2]} ㎥, 난방 {last[3]} h, 온수 {last[4]} h")


def import_stg(paths):
    """지난 허브 통신 기록(hubtrace-*.jsonl: {t, kind, mac, op, x:{mnt:{data:[{label,value}]}}})의 보일러 stg 를 넣는다."""
    c = db()
    added = seen = 0
    for p in paths:
        with open(p, errors="replace") as f:
            for ln in f:
                if '"stg"' not in ln or '"f06"' not in ln:
                    continue
                try:
                    d = json.loads(ln)
                    if d.get("op") != "stg" or d.get("kind") != "f06":
                        continue
                    v = {e["label"]: int(e["value"], 16) for e in d["x"]["mnt"]["data"]}
                    hg, wg, hh, wh = v["d26"], v["d27"], v["d20"], v["d21"]
                except (ValueError, KeyError, TypeError):
                    continue
                seen += 1
                ts = datetime.fromtimestamp(d["t"]).isoformat(timespec="seconds")
                added += add_usage(c, ts, "hubtrace", d.get("mac"), hg, wg, hh, wh)
    c.commit()
    print(f"stg {seen}개 중 {added}개 넣음 (나머지는 이미 있음)")


def to_pg(src):
    """SQLite 기록(src)을 HUB_DB_URL 로. 같은 줄(유일 키)은 건너뛰므로 여러 번 돌려도 된다 — 바꿔 단 뒤 한 번 더 돌리면
    그 사이 SQLite 에만 쌓인 줄이 채워진다. events 의 id 는 Postgres 가 새로 매긴다(시각·boot·seq 가 진짜 키)."""
    if not DB_URL:
        sys.exit("HUB_DB_URL 을 지정하세요")
    if not os.path.exists(src):
        sys.exit(f"{src} 가 없습니다")
    s = sqlite3.connect(f"file:{src}?mode=ro", uri=True)
    d = db()
    print(f"{src} → {d.where()}")
    for t, cols in TABLES.items():
        have = {r[1] for r in s.execute(f"pragma table_info({t})")}
        if not have:
            continue
        pick = ",".join(x if x in have else "null" for x in cols)
        rows = s.execute(f"select {pick} from {t} order by {'id' if t == 'events' else 'ts'}").fetchall()
        before = d.execute(f"select count(*) from {t}").fetchone()[0]
        sql = f"insert into {t}({','.join(cols)}) values({','.join(['%s'] * len(cols))}) on conflict do nothing"
        with d.c.cursor() as cur:
            for i in range(0, len(rows), 5000):
                cur.executemany(sql, rows[i:i + 5000])
        d.commit()
        after = d.execute(f"select count(*) from {t}").fetchone()[0]
        print(f"  {t:13} {len(rows):7}줄 중 {after - before:7}줄 새로 (지금 {after}줄)")


def temps(hours):
    """방별 온도 시간 평균 + 난방(밸브 열림) 비율."""
    c = db()
    since = (datetime.now() - timedelta(hours=hours)).isoformat()
    print(f"== 방 온도 (최근 {hours}시간, 시간 평균 / 난방 중 비율) ==")
    rows = c.execute("""select substr(ts,1,13) h, coalesce(label || ' ', '') || name, avg(cur), avg(setp), avg(heating)
                        from zones where ts>=? group by h, mac, zone, label, name order by h, label, zone""", (since,)).fetchall()
    for h, name, cur, setp, heat in rows:
        print(f"  {h}시  {name:7} {cur:5.1f}°C (설정 {setp:4.1f})  난방 {heat * 100:3.0f}%")
    rows = c.execute("select substr(ts,1,13) h, avg(room_t), avg(room_h) from state where ts>=? and room_t is not null group by h order by h",
                     (since,)).fetchall()
    if rows:
        print("\n-- 온습도계")
        for h, t, rh in rows:
            print(f"  {h}시  {t:5.1f}°C  {rh:4.0f}%")


# ---------------------------------------------------------------- 로그 브라우저 (읽기 전용)

RESET = {0: "알 수 없음", 1: "전원 켬", 2: "외부 리셋", 3: "소프트웨어 재시작", 4: "패닉", 5: "인터럽트 워치독",
         6: "태스크 워치독", 7: "워치독", 8: "딥슬립", 9: "전압 저하", 12: "JTAG", 14: "전원 글리치", 15: "CPU 멈춤"}


def short_mac(mac):
    """이벤트 mac 칸: 기기 MAC 6바이트, 또는 앞 3바이트(끝 3자리 기기 구분)만 쓰고 나머지 0."""
    if not mac:
        return ""
    p = mac.split(":")
    if all(x == "00" for x in p):
        return ""
    return "".join(p[:3]) if len(p) == 6 and p[3:] == ["00", "00", "00"] else mac


def ev_desc(typ, mac, a, bb):
    """이벤트 한 줄을 사람 말로 (펌웨어 hub_events.h 의 칸 뜻)."""
    a = a or 0
    bb = bb or 0
    mac = short_mac(mac)
    if typ == "boot":
        return f"허브 부팅 — {RESET.get(a, f'사유 {a}')}"
    if typ == "ap_join":
        return f"기기 {mac} 가 허브 AP 에 붙음"
    if typ == "ap_leave":
        return f"기기 {mac} 가 허브 AP 에서 떨어짐 — {REASON.get(a, f'사유 {a}')}, {bb / 1000:,.0f}초 붙어 있었음"
    if typ == "sta_up":
        return f"집 Wi-Fi 연결 (채널 {a})"
    if typ == "sta_down":
        return f"집 Wi-Fi 끊김 — {REASON.get(a, f'사유 {a}')}"
    if typ == "link_open":
        return f".{a} 기기가 클라우드 연결 시작"
    if typ in ("link_dev_tls", "link_cloud_tls"):
        side = "기기 쪽" if typ == "link_dev_tls" else "클라우드 쪽"
        return f".{a} {side} TLS " + (f"{bb:,} ms" if bb >= 0 else f"실패 (코드 {bb})")
    if typ == "link_up":
        return f".{a} 기기 클라우드 연결됨"
    if typ == "link_close":
        try:
            who = CLOSER.get(int((mac or "00")[:2], 16), "?")
        except ValueError:
            who = "?"
        mac = None
        return f".{a} 연결 닫힘 — {who}, {bb / 1000:,.0f}초 유지"
    if typ == "link_reject":
        return f".{a} 연결 거절"
    if typ == "bootstrap":
        return f".{a} 부트스트랩 " + ("성공" if bb else "실패")
    if typ == "zb_join_fail":
        return f"Zigbee 가입 실패 (상태 {a}), {bb / 1000:,.0f}초 뒤 다시"
    if typ == "zb_joined":
        return f"Zigbee 가입됨 (PAN {a:04x}, 채널 {bb})"
    if typ == "zb_leave":
        return f"Zigbee 탈퇴 (타입 {a})"
    if typ == "onboard":
        return "온보딩 " + ("성공" if a else "실패")
    if typ == "gas_pulse":
        return f"가스 {bb} L (누적 {a:,} 펄스)"
    if typ == "store_bad":
        return f"저장 데이터 오류 (이번 부팅 {a}건)"
    if typ == "boil_burn":
        mode = {0: "연소 멈춤", 1: "난방 연소", 2: "온수 연소", 3: "난방+온수 연소"}.get(a & 3, str(a))
        return f"보일러 {mode} (가스 누적 {bb:,} 펄스)"
    if typ == "room_out":
        return f"각방 {mac} 존{a + 1} 외출 " + ("켬" if bb else "끔")
    if typ == "gas_no_pulse":
        return f"가스 펄스 없음 경고 — 그동안 연소 {bb:,}초" if a else "가스 펄스 없음 경고 풀림"
    if typ == "cmd_revert":
        return f"명령이 되돌아감 — 기기 {mac} (f0{a:x}), {bb}초 뒤"
    if typ == "boil_stg":
        return (f"보일러 사용량 누계 — 난방 가스 {a & 0xffff} ㎥, 온수 가스 {(a >> 16) & 0xffff} ㎥, "
                f"난방 {bb & 0xffff} h, 온수 {(bb >> 16) & 0xffff} h")
    return ""


def _iso(ms):
    return datetime.fromtimestamp(int(ms) / 1000).isoformat(timespec="seconds")


def _bucket(span_s):
    """기간 길이 → (ts 앞 몇 글자로 묶을지, 한 칸 초). 칸 수가 300~450 안팎이 되게."""
    for lim, n, step in ((8 * 3600, 16, 60), (3 * 86400, 15, 600), (16 * 86400, 13, 3600)):
        if span_s <= lim:
            return n, step
    return 10, 86400


def _key_ms(k):
    k = {16: k, 15: k + "0", 13: k + ":00", 10: k + "T00:00"}[len(k)]
    return int(datetime.fromisoformat(k).timestamp() * 1000)


def _floor(dt, step):
    if step >= 86400:
        return dt.replace(hour=0, minute=0, second=0, microsecond=0)
    if step >= 3600:
        return dt.replace(minute=0, second=0, microsecond=0)
    return dt.replace(minute=dt.minute - dt.minute % (step // 60), second=0, microsecond=0)


def log_meta(c, q):
    since = (datetime.now() - timedelta(days=7)).isoformat(timespec="seconds")
    rooms = [{"mac": m, "label": l, "zone": z, "name": n} for m, l, z, n in c.execute(
        "select mac, max(label), zone, max(name) from zones where ts>=? group by mac, zone order by max(label), mac, zone",
        (since,)).fetchall()]
    first, last = c.execute("select min(ts), max(ts) from state").fetchone()
    types = c.execute("select type, count(*) from events group by type order by 2 desc").fetchall()
    return {"db": "PostgreSQL" if c.pg else "SQLite", "where": c.where(), "hub": HUB_URL, "first": first, "last": last,
            "rooms": rooms, "types": types}


def log_rooms(c, q):
    a, b = _iso(q["from"]), _iso(q["to"])
    n, step = _bucket((int(q["to"]) - int(q["from"])) / 1000)
    out = {}
    # 점 = [칸 시각 ms, 현재, 설정, 난방 비율, 켜짐 비율, 외출 비율] — 비율은 그 칸 안의 기록 중 그 상태였던 몫(0~1)
    for k, mac, zone, cur, setp, heat, on, out_, label, name in c.execute(
            f"""select substr(ts,1,{n}) k, mac, zone, avg(cur), avg(setp), avg(heating), avg(on_), avg(coalesce(outing,0)),
                       max(label), max(name)
                from zones where ts>=? and ts<? group by k, mac, zone order by k""", (a, b)).fetchall():
        r = out.setdefault((mac, zone), {"mac": mac, "zone": zone, "label": label, "name": name, "pts": []})
        r["pts"].append([_key_ms(k), cur, setp, heat, on, out_])
    return {"step": step, "rooms": sorted(out.values(), key=lambda r: (r["label"] or "", r["mac"] or "", r["zone"]))}


def log_boiler(c, q):
    a, b = _iso(q["from"]), _iso(q["to"])
    n, step = _bucket((int(q["to"]) - int(q["from"])) / 1000)
    gas = [[_key_ms(k), l or 0, cnt] for k, l, cnt in c.execute(
        f"select substr(ts,1,{n}) k, sum(b), count(*) from events where type='gas_pulse' and ts>=? and ts<? group by k order by k",
        (a, b)).fetchall()]
    # 연소: boil_burn 은 '바뀐 순간'만 있다 → 기간 앞 마지막 상태부터 구간으로 펴서 칸마다 분으로 나눈다
    evs = c.execute("select ts, a from events where type='boil_burn' and ts<? order by ts desc limit 1", (a,)).fetchall()
    evs += c.execute("select ts, a from events where type='boil_burn' and ts>=? and ts<? order by ts", (a, b)).fetchall()
    t_end = min(datetime.fromisoformat(b), datetime.now())
    burn = {}
    for i, (ts, mode) in enumerate(evs):
        t0 = max(datetime.fromisoformat(ts[:19]), datetime.fromisoformat(a))
        t1 = datetime.fromisoformat(evs[i + 1][0][:19]) if i + 1 < len(evs) else t_end
        while t0 < t1 and mode:
            cell = _floor(t0, step)
            nxt = min(t1, cell + timedelta(seconds=step))
            m = (nxt - t0).total_seconds() / 60
            r = burn.setdefault(int(cell.timestamp() * 1000), [0.0, 0.0])
            if mode & 1:
                r[0] += m
            if mode & 2:
                r[1] += m
            t0 = nxt
    # 보일러 자체 누계(stg): 기간 안의 줄마다 앞 줄과의 차
    rows = c.execute("select ts, heat_gas_m3, hw_gas_m3, heat_burn_h, hw_burn_h from boiler_usage where ts<? order by ts desc limit 1",
                     (a,)).fetchall()
    rows += c.execute("select ts, heat_gas_m3, hw_gas_m3, heat_burn_h, hw_burn_h from boiler_usage where ts>=? and ts<? order by ts",
                      (a, b)).fetchall()
    stg = []
    for p, r in zip(rows, rows[1:]):
        d = [r[i] - p[i] for i in range(1, 5)]
        if min(d) >= 0 and any(d):
            stg.append([int(datetime.fromisoformat(r[0]).timestamp() * 1000)] + d)
    last = rows[-1] if rows else None
    meter = c.execute("select ts, gas_meter_m3, gas_pulses from state where gas_pulses is not null and ts<? order by ts desc limit 1",
                      (b,)).fetchone()
    return {"step": step, "gas": gas, "burn": sorted([k] + v for k, v in burn.items()), "stg": stg,
            "stg_last": last and {"ts": last[0], "heat_gas": last[1], "hw_gas": last[2], "heat_h": last[3], "hw_h": last[4]},
            "meter": meter and {"ts": meter[0], "m3": meter[1], "pulses": meter[2]}}


def log_hub(c, q):
    a, b = _iso(q["from"]), _iso(q["to"])
    n, step = _bucket((int(q["to"]) - int(q["from"])) / 1000)
    polls = [[_key_ms(k), cnt, int(ok or 0)] for k, cnt, ok in c.execute(
        f"select substr(ts,1,{n}) k, count(*), sum(ok) from polls where ts>=? and ts<? group by k order by k", (a, b)).fetchall()]
    st = [[_key_ms(k), hm, rssi] for k, hm, rssi in c.execute(
        f"select substr(ts,1,{n}) k, min(heap_min), avg(rssi) from state where ts>=? and ts<? group by k order by k", (a, b)).fetchall()]
    boots = [[int(datetime.fromisoformat(ts[:19]).timestamp() * 1000), ev_desc("boot", None, aa, None)] for ts, aa in c.execute(
        "select ts, a from events where type='boot' and ts>=? and ts<? order by ts", (a, b)).fetchall()]
    errs = c.execute("select err, count(*) from polls where ok=0 and ts>=? and ts<? group by err order by 2 desc limit 8",
                     (a, b)).fetchall()
    return {"step": step, "polls": polls, "state": st, "boots": boots, "errors": errs}


def log_events(c, q):
    a, b = _iso(q["from"]), _iso(q["to"])
    where, args = ["ts>=?", "ts<?"], [a, b]
    types = [t for t in (q.get("types") or "").split(",") if re.fullmatch(r"[a-z_]{1,24}", t)]
    if types:
        where.append("type in (" + ",".join("?" * len(types)) + ")")
        args += types
    if q.get("before"):
        where.append("ts<?")
        args.append(q["before"])
    w = " and ".join(where)
    limit = max(1, min(int(q.get("limit") or 200), 1000))
    rows = c.execute(f"select ts, type, mac, a, b from events where {w} order by ts desc limit {limit}", args).fetchall()
    counts = c.execute("select type, count(*) from events where ts>=? and ts<? group by type order by 2 desc", (a, b)).fetchall()
    return {"rows": [[ts, t, "" if t == "link_close" else short_mac(m), x, y, ev_desc(t, m, x, y)] for ts, t, m, x, y in rows],
            "counts": counts,
            "more": len(rows) == limit}


LOG_API = {"meta": log_meta, "rooms": log_rooms, "boiler": log_boiler, "hub": log_hub, "events": log_events}


# ---------- 로그 브라우저 로그인 ----------
# 아이디·암호는 WEB_AUTH 파일 하나(0600)에: 암호는 PBKDF2-SHA256(소금 16바이트, 30만 번)으로만 둔다.
# 로그인하면 서명한 쿠키(아이디|만료|난수 + HMAC)를 30일 준다. 서명 키도 그 파일에 있어 파일을 지우면 모두 로그아웃.
import base64
import hashlib
import hmac
import secrets

AUTH_ITER = 300_000
SESSION_DAYS = 30
COOKIE = "hubweb"
FAIL_MAX, FAIL_WIN = 5, 900          # 한 주소에서 15분에 5번 틀리면 그 15분이 지날 때까지 막는다
_fails = {}
_fails_lock = threading.Lock()


def _pw_hash(pw, salt, n=AUTH_ITER):
    return hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, n).hex()


def auth_load():
    try:
        with open(WEB_AUTH) as f:
            a = json.load(f)
        return a if a.get("user") and a.get("hash") and a.get("secret") else None
    except (OSError, ValueError):
        return None


def auth_create(user, pw):
    """처음 한 번만. 이미 있으면 False(동시에 두 사람이 만들려 해도 먼저 쓴 쪽만)."""
    salt = secrets.token_bytes(16)
    a = {"user": user, "salt": salt.hex(), "iter": AUTH_ITER, "hash": _pw_hash(pw, salt),
         "secret": secrets.token_hex(32), "created": datetime.now().isoformat(timespec="seconds")}
    os.makedirs(os.path.dirname(WEB_AUTH) or ".", exist_ok=True)
    try:
        fd = os.open(WEB_AUTH, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        return False
    with os.fdopen(fd, "w") as f:
        json.dump(a, f, ensure_ascii=False)
    return True


def auth_check(a, user, pw):
    ok_user = hmac.compare_digest(user.encode(), a["user"].encode())
    got = _pw_hash(pw, bytes.fromhex(a["salt"]), int(a.get("iter", AUTH_ITER)))   # 아이디가 틀려도 같은 시간
    return ok_user and hmac.compare_digest(got, a["hash"])


def _sign(a, body):
    return hmac.new(bytes.fromhex(a["secret"]), body.encode(), hashlib.sha256).hexdigest()


def session_make(a):
    body = f'{base64.urlsafe_b64encode(a["user"].encode()).decode()}.{int(time.time()) + SESSION_DAYS * 86400}.{secrets.token_hex(8)}'
    return f"{body}.{_sign(a, body)}"


def session_ok(a, token):
    try:
        u64, exp, nonce, sig = token.split(".")
    except (AttributeError, ValueError):
        return False
    body = f"{u64}.{exp}.{nonce}"
    if not hmac.compare_digest(sig, _sign(a, body)):
        return False
    try:
        return int(exp) > time.time() and base64.urlsafe_b64decode(u64).decode() == a["user"]
    except ValueError:
        return False


def fail_blocked(ip):
    now = time.time()
    with _fails_lock:
        ts = [t for t in _fails.get(ip, []) if now - t < FAIL_WIN]
        _fails[ip] = ts
        return len(ts) >= FAIL_MAX


def fail_note(ip, ok):
    with _fails_lock:
        if ok:
            _fails.pop(ip, None)
        else:
            _fails.setdefault(ip, []).append(time.time())


LOGIN_HTML = """<!doctype html>
<html lang="ko"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>허브 기록 · 로그인</title>
<style>
:root { color-scheme: light; --page:#f9f9f7; --surface:#fcfcfb; --ink:#0b0b0b; --ink2:#52514e; --muted:#898781;
  --border:rgba(11,11,11,.14); --focus:#2a78d6; --crit:#d03b3b; --brand:#f55600; }
@media (prefers-color-scheme: dark) { :root { color-scheme: dark; --page:#0d0d0d; --surface:#1a1a19; --ink:#f0efec;
  --ink2:#c3c2b7; --muted:#898781; --border:rgba(255,255,255,.14); --focus:#3987e5; } }
* { box-sizing: border-box; }
body { margin:0; min-height:100vh; display:grid; place-items:center; background:var(--page); color:var(--ink);
  font:14px/1.5 system-ui,-apple-system,"Segoe UI","Apple SD Gothic Neo","Malgun Gothic",sans-serif;
  padding:24px max(16px,env(safe-area-inset-right)) 24px max(16px,env(safe-area-inset-left)); }
main { width:100%; max-width:340px; background:var(--surface); border:1px solid var(--border); border-radius:12px; padding:28px 24px 24px; }
.head { display:flex; align-items:center; gap:10px; margin-bottom:6px; }
.head svg { width:32px; height:32px; flex:none; }
h1 { font-size:18px; margin:0; font-weight:650; letter-spacing:-.01em; }
p.lead { color:var(--ink2); margin:0 0 18px; font-size:13px; }
label { display:block; font-size:12.5px; color:var(--ink2); margin:12px 0 4px; }
input { width:100%; font:inherit; font-size:15px; padding:9px 11px; border:1px solid var(--border); border-radius:8px;
  background:var(--page); color:var(--ink); }
input:focus { outline:2px solid var(--focus); outline-offset:-1px; border-color:transparent; }
.hint { color:var(--muted); font-size:12px; margin-top:4px; }
button { width:100%; margin-top:20px; padding:10px; font:inherit; font-weight:600; border:0; border-radius:8px;
  background:var(--brand); color:#fff; cursor:pointer; }
button:hover { filter:brightness(1.06); }
.err { color:var(--crit); font-size:13px; margin:14px 0 0; }
</style></head>
<body><main>
<div class="head">%ICON%<h1>허브 기록</h1></div>
<p class="lead">%LEAD%</p>
<form method="post" action="login" autocomplete="on">
<input type="hidden" name="mode" value="%MODE%">
<label for="u">아이디</label>
<input id="u" name="user" value="%USER%" autocomplete="username" autocapitalize="off" spellcheck="false" required maxlength="32" autofocus>
<label for="%PWNAME%">%PWLABEL%</label>
<input id="%PWNAME%" name="%PWNAME%" type="password" autocomplete="%PWAC%" required %PWMIN%>
%CONFIRM%
%ERR%
<button type="submit">%BUTTON%</button>
</form>
</main></body></html>
"""

# 만들기 화면: 1Password 등이 첫 칸을 '지금 암호'로 잘못 읽지 않게 이름·id 를 new-password / confirm-password 로,
# 둘 다 autocomplete=new-password + passwordrules 를 단다(그래야 첫 칸에서 암호 생성을 띄우고 확인 칸도 같이 채운다)
LOGIN_CONFIRM = """<div class="hint" id="pwhint">8자 이상</div>
<label for="confirm-password">새 암호 확인</label>
<input id="confirm-password" name="confirm-password" type="password" autocomplete="new-password" required minlength="8"
 passwordrules="minlength: 8;">"""


def login_page(setup, user="", err=""):
    from html import escape
    v = {"ICON": ICON_SVG, "MODE": "setup" if setup else "login", "USER": escape(user),
         "LEAD": "처음 오셨습니다. 이 화면에 쓸 아이디와 암호를 만드세요. 만든 뒤에는 이 아이디로만 들어올 수 있습니다."
                 if setup else "로그인하세요.",
         "PWAC": "new-password" if setup else "current-password",
         "PWNAME": "new-password" if setup else "password", "PWLABEL": "새 암호" if setup else "암호",
         "PWMIN": 'minlength="8" passwordrules="minlength: 8;" aria-describedby="pwhint"' if setup else "",
         "CONFIRM": LOGIN_CONFIRM if setup else "", "BUTTON": "만들고 들어가기" if setup else "로그인",
         "ERR": f'<p class="err" role="alert">{escape(err)}</p>' if err else ""}
    out = LOGIN_HTML
    for k, val in v.items():
        out = out.replace(f"%{k}%", val)
    return out.encode()


ICON_SVG = '<svg viewBox="0 0 256 256" aria-hidden="true"><rect width="256" height="256" rx="56" fill="#f55600"/><g fill="none" stroke="#fff" stroke-width="11" stroke-linecap="round" stroke-linejoin="round"><path d="M150.5 203 204.2 172V84L128 40 51.8 84v88L110 205.6C124 213.7 128 200 128 178C107.2 178 91.2 162 91.2 141.2C91.2 123.6 100.8 112.4 110.4 102.8C110.4 115.6 115.2 123.6 123.2 125.2C118.4 107.6 123.2 88.4 132 74C136 96.4 164.8 110.8 164.8 141.2C164.8 158.8 153.6 171.6 139.2 173.2"/><path d="M139.2 173.2C128 174.8 118.4 166.8 118.4 155.6C118.4 146 126.4 141.2 129.6 131.6"/></g></svg>'



def start_web(port, block=False):
    """로그 브라우저. 읽기만 한다. DB 연결은 하나를 잠가서 같이 쓴다(드물게 오는 요청이라 충분)."""
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    from urllib.parse import parse_qsl, urlparse
    lock = threading.Lock()
    conn = [None]

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, body, ctype, headers=()):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Frame-Options", "DENY")
            for k, v in headers:
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def go(self, where, cookie=None):
            hs = [("Location", where)]
            if cookie is not None:
                hs.append(("Set-Cookie", f"{COOKIE}={cookie}; Path=/; HttpOnly; SameSite=Strict"
                                         + (f"; Max-Age={SESSION_DAYS * 86400}" if cookie else "; Max-Age=0")))
            self.send(303, b"", "text/plain", hs)

        def session(self):
            a = auth_load()
            if not a:
                return None
            for part in (self.headers.get("Cookie") or "").split(";"):
                k, _, v = part.strip().partition("=")
                if k == COOKIE and session_ok(a, v):
                    return a
            return None

        def same_origin(self):
            o = self.headers.get("Origin")
            return not o or o == "null" or urlparse(o).netloc == self.headers.get("Host")

        def do_POST(self):
            u = urlparse(self.path)
            n = min(int(self.headers.get("Content-Length") or 0), 4096)
            form = dict(parse_qsl(self.rfile.read(n).decode("utf-8", "replace")))
            if not self.same_origin():
                return self.send(403, b"cross-origin", "text/plain")
            if u.path.endswith("/logout"):
                return self.go("login", "")
            if not u.path.endswith("/login"):
                return self.send(404, b"not found", "text/plain")
            ip = self.client_address[0]
            user = form.get("user", "").strip()
            pw = form.get("new-password", form.get("password", ""))
            a = auth_load()
            html = "text/html; charset=utf-8"
            if a is None:                                        # 처음: 만들기
                err = ("아이디를 1~32자로, 띄어쓰기 없이 적어 주세요." if not re.fullmatch(r"\S{1,32}", user)
                       else "암호는 8자 이상이어야 합니다." if len(pw) < 8
                       else "암호 확인이 다릅니다." if pw != form.get("confirm-password", "")
                       else "")
                if err:
                    return self.send(400, login_page(True, user, err), html)
                if not auth_create(user, pw):
                    return self.send(409, login_page(False, user, "방금 다른 곳에서 먼저 만들었습니다. 그 아이디로 로그인하세요."), html)
                print(f"로그 브라우저: 로그인 정보 만듦({ip}, 아이디 {user})", flush=True)
                return self.go("./", session_make(auth_load()))
            if fail_blocked(ip):
                return self.send(429, login_page(False, user, "여러 번 틀려 15분 동안 막았습니다. 잠시 뒤 다시 해 주세요."), html)
            ok = auth_check(a, user, pw)
            fail_note(ip, ok)
            if not ok:
                print(f"로그 브라우저: 로그인 실패({ip})", flush=True)
                return self.send(401, login_page(False, user, "아이디나 암호가 틀렸습니다."), html)
            return self.go("./", session_make(a))

        def do_GET(self):
            u = urlparse(self.path)
            if u.path.endswith("/login"):
                if self.session():
                    return self.go("./")
                return self.send(200, login_page(auth_load() is None), "text/html; charset=utf-8")
            if not self.session():
                if u.path.startswith("/api/"):
                    return self.send(401, b'{"error":"login"}', "application/json")
                return self.go("login")
            if u.path in ("/", "/index.html"):
                try:
                    with open(WEB_HTML, "rb") as f:                 # 요청마다 읽는다(배포하면 바로 바뀜)
                        return self.send(200, f.read(), "text/html; charset=utf-8")
                except OSError:
                    return self.send(404, b"lynnai_hub_web.html not found", "text/plain")
            name = u.path[len("/api/log/"):] if u.path.startswith("/api/log/") else None
            if name not in LOG_API:
                return self.send(404, b"not found", "text/plain")
            q = dict(parse_qsl(u.query))
            now = int(time.time() * 1000)
            q.setdefault("to", str(now))
            q.setdefault("from", str(int(q["to"]) - 86400000))
            with lock:
                try:
                    if conn[0] is None:
                        conn[0] = db()
                    out = LOG_API[name](conn[0], q)
                    conn[0].commit()           # Postgres 는 읽기만 해도 트랜잭션이 열린다 — 닫아 둔다
                except Exception as e:
                    if conn[0] is not None:
                        conn[0].recover()
                    return self.send(500, json.dumps({"error": str(e)}, ensure_ascii=False).encode(), "application/json")
            self.send(200, json.dumps(out, ensure_ascii=False, separators=(",", ":")).encode(), "application/json")

    ThreadingHTTPServer.allow_reuse_address = True
    srv = ThreadingHTTPServer(("0.0.0.0", port), H)
    srv.daemon_threads = True
    print(f"로그 브라우저: http://0.0.0.0:{port}/", flush=True)
    if block:
        srv.serve_forever()
    else:
        threading.Thread(target=srv.serve_forever, daemon=True).start()


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    arg = float(sys.argv[2]) if len(sys.argv) > 2 and cmd not in ("import-stg", "to-pg", "web") else None
    if cmd == "summary":
        summary(arg or 24)
    elif cmd == "gas":
        gas(int(arg or 14))
    elif cmd == "temps":
        temps(arg or 24)
    elif cmd == "burn":
        burn(arg or 24)
    elif cmd == "usage":
        usage(int(arg or 7))
    elif cmd == "import-stg":
        import_stg(sys.argv[2:])
    elif cmd == "web":
        start_web(int(sys.argv[2]) if len(sys.argv) > 2 else (WEB_PORT or 8497), block=True)
    elif cmd == "to-pg":
        to_pg(sys.argv[2] if len(sys.argv) > 2 else DB)
    else:
        main()
