# lynnai-hub

린나이(Rinnai) 프로토콜을 쓰는 보일러·각방 조절기를 **클라우드 없이 로컬에서** 제어하는 허브.
라즈베리 파이 한 대가 기기 입장에서의 "클라우드"가 되어, 집 안에서 상태를 읽고 명령을 내리고
Home Assistant / SmartThings 로 중계한다.

> ⚠️ 리버스 엔지니어링으로 만든 **비공식** 프로젝트입니다. 린나이와 아무 관련이 없고
> 후원·보증받지 않았습니다. `lynnai` 는 상표 회피를 위한 임시 명칭입니다.
> 본인 소유 기기에 대해 본인 책임으로 사용하세요.

## 구성

| 디렉터리 | 역할 |
|---|---|
| `standalone/` | 클라우드를 **대체**하는 로컬 MQTT 브로커 + DNS 하이재킹 + Wi-Fi AP. 기기가 린나이 클라우드 대신 여기에 붙는다. (Pi3 기준, Pico 포팅 지향) |
| `appliance/` | 배포 이미지 **제품**: 첫 부팅 자가 개별화(공유 비밀 0 원칙), 프로비저닝/관리 웹 포털(설정 Wi-Fi 접속 시 설정 페이지 자동 표시 = **캡티브 포털**), 기기 레지스트리(보일러·각방·기타 린나이 기기), standalone/proxy 모드 전환 |
| `plugins/gasmeter/` | *(선택 플러그인)* 가스 계량기 리드스위치 펄스 카운터 + 도시가스 요금 계산. HA Energy 대시보드로 사용량·요금 송신 |
| `plugins/roomsensor/` | *(선택 플러그인)* SHT31-D 온습도계(I2C 0x44) → 로컬 MQTT + HA 자동 등록(온도 °C · 습도 %) |

## 동작 모드

- **standalone** (기본) — 기기가 우리 브로커에 붙는다. 클라우드·계정·외부 키 불필요.
- **proxy** — 실제 린나이 클라우드로 중계하며 중간에서 관찰/제어. 앱 키·계정은 포털에서 설정(레포에 없음).

## 보안 설계

- **공유 비밀 0**: 배포 이미지는 전부 동일하게 구워지고, 각 기기는 첫 부팅에 CPU 시리얼 + HW RNG 로
  자기만의 비밀을 만든다. 이미지 한 장을 뜯어도 다른 기기는 안전.
- 첫 설정 AP 는 공개(개방)망 → 포털에서 생산 SSID/WPA 와 관리자 암호를 강제 설정, 생산 AP 암호는
  기기별 랜덤 생성.
- 관리 포털은 **AP 인터페이스에만** 바인드(가정용 Wi-Fi 노출 금지), 세션은 기기별 키로 HMAC 서명.
- 린나이 앱 키·클라우드 인증서는 **bring-your-own**: `.env` 로 주입하며 저장소에 넣지 않는다.

상태: WIP(진행 중). 자세한 내용은 각 디렉터리의 README / 주석 참고.

---

## English

**lynnai-hub** — a local, cloud‑free control hub for Rinnai‑protocol boilers and room controllers.
A single Raspberry Pi becomes the "cloud" the appliance talks to, so you read state and send commands
on your own LAN and bridge them to Home Assistant / SmartThings.

> ⚠️ Unofficial, reverse‑engineered project. Not affiliated with, endorsed by, or supported by Rinnai.
> `lynnai` is a placeholder name to avoid trademark use. Use on hardware you own, at your own risk.

- `standalone/` — a local MQTT broker + DNS hijack + Wi‑Fi AP that **replaces** the cloud; the device
  connects here instead of Rinnai's servers.
- `appliance/` — the distributable image product: first‑boot self‑individualization (zero shared
  secrets), a provisioning/management web portal, a device registry, and standalone/proxy modes.
- `plugins/gasmeter/` — *(optional)* a gas‑meter reed‑switch pulse counter + city‑gas pricing that
  reports usage and cost to the Home Assistant Energy dashboard.
- `plugins/roomsensor/` — *(optional)* an SHT31‑D temperature/humidity sensor (I2C) that publishes to
  the local broker and auto‑registers in Home Assistant.

License: MIT (see `LICENSE`).
