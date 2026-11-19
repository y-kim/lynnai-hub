# lynnai-hub-watch

LYNNAI 허브의 장기 기록기. 허브가 여는 서버 인터페이스를 읽어 SQLite(`data/hub.db`)에 쌓는다.
표준 라이브러리만 쓰므로 파이썬 3.8+ 어디서나 돈다.

## 허브 서버 인터페이스 (펌웨어 `main/hub_api.c`, `main/hub_events.c`)

| 경로 | 내용 |
|---|---|
| `GET /api/v1/state` | 지금 상태 전체(JSON): 허브(힙·RSSI), 연결, 보일러(해독 값 + `raw`), 각방, 온습도계, 가스 누적 |
| `GET /metrics` | 같은 값의 Prometheus 텍스트 형식 (`lynnai_*`) — Prometheus/Grafana 로 바로 긁기 |
| `GET /api/events?since=N` | 이벤트 링(256): 연결 끊김, Zigbee, `gas_pulse`(a=누적, b=1펄스 L) |
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
```

표: `state`(1분마다 한 줄), `zones`(방별), `events`(허브 이벤트), `polls`(허브 응답 여부).
그래프는 이 DB 를 Grafana(SQLite 데이터 소스)로 보거나, 허브 `/metrics` 를 Prometheus 로 따로 긁는다.
