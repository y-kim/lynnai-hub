# lynnai-hub-watch

LYNNAI 허브의 장기 기록기. 허브가 여는 서버 인터페이스를 읽어 SQLite 파일(`data/hub.db`) 또는 PostgreSQL 에 쌓는다.
SQLite 로 쓰면 표준 라이브러리만 쓰므로 파이썬 3.8+ 어디서나 돈다. PostgreSQL 은 `psycopg` 3 가 필요하다(Dockerfile 에 들어 있음).

## 저장소 고르기

- **SQLite(기본):** `HUB_DB`(기본 `data/hub.db`) 파일 하나.
- **PostgreSQL:** `HUB_DB_URL=postgresql://계정:암호@호스트:포트/DB` 를 주면 그쪽에 쓴다. 표는 처음 뜰 때 만든다.
  기록기 전용 계정이 DB 를 소유하게 만들면 된다(`create role … login password …; create database … owner …;`).
- **옮기기:** `HUB_DB_URL=… python lynnai_hub_watch.py to-pg [data/hub.db]` — SQLite 기록을 Postgres 로 복사한다.
  같은 줄은 건너뛰므로 여러 번 돌려도 된다. 순서: ① to-pg ② 기록기를 `HUB_DB_URL` 로 다시 띄움
  ③ to-pg 한 번 더(그 사이 SQLite 에만 쌓인 줄 채움). 이벤트는 허브 버퍼(256개)에서 다시 받아 오므로 빠지지 않는다.

## 로그 브라우저

`WEB_PORT`(예 8497)를 주면 기록기가 읽기 전용 웹 화면을 같이 띄운다: `http://NAS주소:8497`.
탭 넷 — **방 온도**(층·방별 현재/설정 온도와 난방수 공급), **보일러·가스**(계량기 펄스, 난방·온수 연소 시간,
보일러 자체 누계), **이벤트**(종류별로 걸러 보는 허브 이벤트, 사람 말 설명), **허브 연결**(응답, 재부팅, 메모리, Wi-Fi).
기간은 6시간~30일, 앞뒤로 옮길 수 있고 '지금'이면 1분마다 새로 읽는다. 화면은 `lynnai_hub_web.html`(요청마다 읽음),
데이터는 `GET /api/log/{meta,rooms,boiler,events,hub}?from=&to=`(밀리초).

**로그인:** 처음 들어오면 아이디·암호(8자 이상)를 만드는 화면이 나오고, 그 뒤로는 그 아이디로 로그인해야 화면·API 가 열린다
(로그인 안 한 API 요청은 401). 정보는 `data/hubweb-auth.json`(0600, 환경변수 `HUB_WEB_AUTH` 로 바꿀 수 있음)에
암호 해시(PBKDF2-SHA256 30만 번)와 세션 서명 키만 둔다. 세션은 30일짜리 서명 쿠키(HttpOnly·SameSite=Strict),
한 주소에서 15분에 5번 틀리면 15분 막는다. **아이디·암호를 잊으면 그 파일을 지운다** → 다음 접속에서 다시 만들기(모든 세션도 끝남).
평문 HTTP 라 암호가 그대로 오가니 여전히 **LAN 안에서만** 연다.

기록 없이 화면만 띄우려면 `python lynnai_hub_watch.py web [포트]`.

- 시각 칸 `ts` 는 두 DB 모두 ISO 글자다(Postgres 는 `collate "C"`). Grafana 등에서는 `ts::timestamp` 로 쓴다.

## 허브 서버 인터페이스 (펌웨어 `main/hub_api.c`, `main/hub_events.c`)

| 경로 | 내용 |
|---|---|
| `GET /api/v1/state` | 지금 상태 전체(JSON): 허브(힙·RSSI), 연결, 보일러(해독 값 + `raw`), 각방, 온습도계, 가스 누적 |
| `GET /metrics` | 같은 값의 Prometheus 텍스트 형식 (`lynnai_*`) — Prometheus/Grafana 로 바로 긁기 |
| `GET /api/events?since=N` | 이벤트 링(256): 연결 끊김, Zigbee, `gas_pulse`(a=누적, b=1펄스 L), `boil_stg`(보일러 사용량 누계, 아래) |
| `GET /api/stats` | 이벤트 종류별 누적 횟수, 부팅 id, 힙 |

허브 관리 화면(`/admin` → 서버 연동)에서 API 토큰을 만들면 위 경로 모두 `Authorization: Bearer <토큰>` 이 필요하다.
`state.time` 은 허브가 SNTP 로 맞춘 유닉스 시각(맞추기 전엔 `null`), 이벤트는 부팅 후 ms(`up`)라
기록기가 `받은 시각 - (응답 up - 이벤트 up)` 으로 실제 시각을 계산한다.

가스 사용량은 누적 카운터다. 아무 간격으로 읽어도 차이를 내면 그 사이 사용량이 정확히 나온다.

## 기록기

```sh
HUB_URL=http://<허브> HUB_TOKEN=<토큰> python lynnai_hub_watch.py   # 기록
python lynnai_hub_watch.py summary 24   # 연결 품질
python lynnai_hub_watch.py gas 14       # 날짜별 가스 사용량
python lynnai_hub_watch.py temps 24     # 방별 온도·난방 비율
python lynnai_hub_watch.py usage 7      # 보일러 자체 사용량(stg) 날짜별·시간별
python lynnai_hub_watch.py import-stg hubtrace-*.jsonl   # 지난 허브 통신 기록의 stg 를 넣기
```

표: `state`(1분마다 한 줄), `zones`(조절기 mac·층 라벨·방별 — 현재·설정 온도, 켜짐, 난방수 공급, 외출, 예약, 시간 난방 간격 원값 `ivl`), `events`(허브 이벤트), `polls`(허브 응답 여부),
`boiler_usage`(보일러가 약 1시간마다 올리는 누계 한 줄씩 — 난방·온수 가스 ㎥, 버너 시간, 모두 정수. 그 시간 사용량은 앞 줄과의 차).

`boiler_usage` 는 허브의 `boil_stg` 이벤트(a = 난방 가스 | 온수 가스 << 16, b = 난방 시간 | 온수 시간 << 16)에서 온다.
허브가 프록시든 단독이든 보일러의 `stg` 를 직접 받으므로 클라우드가 필요 없다. 기록기는 자기 파일이 바뀌면 스스로 다시 뜬다.
그래프는 이 DB 를 Grafana(SQLite·PostgreSQL 데이터 소스)로 보거나, 허브 `/metrics` 를 Prometheus 로 따로 긁는다.
