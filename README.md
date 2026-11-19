# lynnai-hub

린나이(Rinnai) 프로토콜을 쓰는 보일러 조절기·각방 온도 조절기를 **집 안에서** 제어하고 SmartThings 에 붙이는 허브.
ESP32-C6 보드 하나가 기기 입장에서의 "클라우드"가 되어, 기기를 자기 Wi-Fi(AP)로 받아 상태를 읽고 명령을 넣는다.
린나이 클라우드로 그대로 이어 주거나(프록시 모드, 린나이 앱 계속 사용) 클라우드 없이 혼자 동작한다(단독 모드).
SmartThings 와는 Zigbee(또는 LAN)로 붙는다.

> ⚠️ 리버스 엔지니어링으로 만든 **비공식** 프로젝트입니다. 린나이와 아무 관련이 없고
> 후원·보증받지 않았습니다. `lynnai` 는 상표 회피를 위한 임시 명칭입니다.
> 본인 소유 기기에 대해 본인 책임으로 사용하세요.

```
보일러 조절기 ─┐ Wi-Fi(허브 AP)          ┌─ 린나이 클라우드 (프록시 모드)
온도 조절기  ─┴→ [ESP32-C6 허브] ──집 Wi-Fi─┘
                       └─ Zigbee ─→ SmartThings 허브/Station ─→ SmartThings 앱
```

## 빠른 시작

자세한 내용은 **[docs/esp32-hub.md](docs/esp32-hub.md)** (설치·연결·초기화·고장 분석, 빌드는 부록).

1. **펌웨어 쓰기**: ESP32-C6-DevKitC-1 에 릴리즈 이미지를 씁니다(esptool 또는 브라우저 웹 플래셔).
2. **첫 설정**: 허브가 띄우는 `LYNNAI-SETUP-…` AP 에 휴대폰으로 붙어 집 Wi-Fi·허브 AP·관리자 암호를 넣습니다.
3. **SmartThings**: 초대 링크 <https://bestow-regional.api.smartthings.com/invite/Qrjp9gnaJbMq> 로 드라이버
   *LYNNAI Hub (Zigbee)* 를 설치 → 앱에서 **기기 추가 → 주변 기기 검색** → 허브 버튼 **짧게**.
4. **린나이 기기**: 보일러·온도 조절기 Wi-Fi 모듈을 설정 모드로 두고 허브 버튼 **길게(3초)**.

## 주요 기능

- **SmartThings 기기**: 보일러(전원·난방 모드/방식/온도·온수·예열·예약 상태), 온도 조절기가 알려 주는 방마다
  기기(방1~방6·거실: 켜기·설정 온도·외출·예약), 온습도계, 가스 계량기. 보일러·온도 조절기 여러 대와 짝 지정 지원.
- **자동화(드라이버 내장)**: 모든 방이 꺼지면 보일러 난방을 끄고, 방이 켜지면 다시 켠다.
- **관리 화면** `http://<허브IP>/admin`: 동작 모드, 연결 방식(Zigbee/LAN), 연결된 기기(이름·보일러 연결·기기/방별 노출),
  종류별 노출 컨트롤, 서버 연동(API 토큰), 센서 플러그인.
- **서버 연동**: `/api/v1/state`(JSON)·`/metrics`(Prometheus)·`/api/events`·`/api/stats`, NAS 기록기 `server/lynnai-hub-watch`.
- **고장 진단**: LED 오류 코드(빨강 N번 깜빡), USB 시리얼 `diag`, PC 진단 스크립트 `tools/hub_doctor.py`.

## 동작 모드

- **프록시** (기본) — 기기 ↔ 허브 ↔ 린나이 클라우드를 이어 준다. 린나이 앱을 그대로 쓰면서 SmartThings 로도 제어.
  클라우드 주소는 허브가 집 Wi-Fi 쪽 DNS 로 직접 조회한다.
- **단독** — 클라우드에 붙지 않고 허브가 기기의 MQTT 상대가 된다. 인터넷이 없어도 SmartThings 로 제어되지만
  린나이 앱으로는 보이지 않는다.

관리 화면 맨 위에서 고르고, 바꾸면 기기가 다시 접속할 때(수십 초) 적용된다.

## 구성

| 디렉터리 | 역할 |
|---|---|
| `firmware/esp32/` | **ESP32-C6 허브 펌웨어**(ESP-IDF v5.5): 허브 AP·DNS·TLS 중계/단독 브로커·Zigbee·LAN·관리 화면·API·센서·진단 |
| `edge-driver/` | SmartThings Edge 드라이버(Zigbee). 공통 로직 `src/core.lua`, 하드웨어 없는 시험 `test/` |
| `edge-driver-lan/` | 같은 코어를 쓰는 LAN 드라이버(SSDP + TCP 8484) |
| `docs/` | 설치·운영 안내 |
| `tools/hub_doctor.py` | PC 에서 허브 고장 진단(API · SmartThings CLI · USB 시리얼) |
| `server/lynnai-nas/` | ESP32 없이 **NAS 도커**로 같은 일을 하는 LAN 전용 허브(파이썬 판, 프록시/단독) |
| `server/lynnai-hub-watch/` | 허브 API 를 SQLite 로 모으는 NAS 기록기 |
| `standalone/` | 클라우드를 대체하는 최소 MQTT 브로커 + DNS 하이재킹(Pi 시절 검증용, NAS 단독 모드가 재사용) |
| `appliance/`, `plugins/` | 라즈베리 파이 판 제품(프로비저닝 포털·플러그인). ESP32 허브로 옮겨 오며 지금은 참고용 |

## 보안

- **펌웨어 이미지에 비밀값이 없다**: 기기 접속용 TLS 인증서·키는 허브마다 첫 부팅 때 직접 만든다(RSA-2048).
  펌웨어에 들어 있는 인증서는 공개 정보인 린나이 클라우드 CA 뿐이다.
- **클라우드 검증**: 허브(와 NAS 프록시)는 린나이 클라우드 인증서를 사설 CA "ckstack" 으로 검증한다.
- **첫 설정 AP** 는 개방망 → 설정 페이지에서 허브 AP 암호(WPA2)와 관리자 암호를 반드시 정한다.
  관리자 암호는 PBKDF2-HMAC-SHA256 해시로만 저장하고, 로그아웃은 세션을 확인한 POST 로만 된다.
- **저장 데이터 검사**: 설정마다 CRC32 를 같이 저장해 손상을 찾아 알린다(LED 4번).
- **한계**: eFuse 를 쓰지 않아 플래시·NVS 암호화가 없다. USB 로 플래시를 읽으면 집 Wi-Fi 암호·허브 AP 암호·API 토큰이
  보인다. USB 진단 `diag` 는 비밀값을 출력하지 않는다. 기기(보일러 모듈)는 허브 인증서를 검증하지 않는다.

상태: WIP(진행 중). 자세한 내용은 [docs/esp32-hub.md](docs/esp32-hub.md) 와 각 디렉터리의 README / 주석 참고.

---

## English

**lynnai-hub** — a local hub for Rinnai‑protocol boiler controllers and room thermostats, bridged to SmartThings.
A single ESP32‑C6 board becomes the "cloud" the appliances talk to: they join the hub's own Wi‑Fi AP, and the hub
reads their state and injects commands. It either relays them to the real Rinnai cloud (proxy mode, the official app
keeps working) or works on its own (standalone mode), and talks to SmartThings over Zigbee (or LAN).

> ⚠️ Unofficial, reverse‑engineered project. Not affiliated with, endorsed by, or supported by Rinnai.
> `lynnai` is a placeholder name to avoid trademark use. Use on hardware you own, at your own risk.

- `firmware/esp32/` — the ESP32‑C6 hub firmware (ESP‑IDF v5.5). Setup and troubleshooting:
  [docs/esp32-hub.md](docs/esp32-hub.md) (Korean).
- `edge-driver/`, `edge-driver-lan/` — SmartThings Edge drivers (Zigbee / LAN) sharing `edge-driver/src/core.lua`.
  Install via the channel invite <https://bestow-regional.api.smartthings.com/invite/Qrjp9gnaJbMq>.
- `server/lynnai-nas/` — the same hub as a LAN‑only Docker container for a NAS; `server/lynnai-hub-watch/` records
  the hub API into SQLite.
- `tools/hub_doctor.py` — diagnostics from a PC (API, SmartThings CLI, USB serial).
- `standalone/`, `appliance/`, `plugins/` — the earlier Raspberry Pi version, kept for reference.

The firmware image carries no secrets: each hub generates its own device‑facing TLS key on first boot, and verifies
the Rinnai cloud against its private CA. There is no flash/NVS encryption (eFuses are not used).

License: MIT (see `LICENSE`).
