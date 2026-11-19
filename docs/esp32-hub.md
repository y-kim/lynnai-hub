# ESP32 허브 설치·운영 안내

ESP32-C6 보드 하나로 린나이 보일러 조절기·온도 조절기를 SmartThings 에 붙이는 허브의
펌웨어 쓰기, 첫 설정, 연결, 초기화, 고장 분석 방법을 정리했습니다.

```
보일러 조절기 ─┐ Wi-Fi(허브 AP)          ┌─ 린나이 클라우드 (프록시 모드)
온도 조절기  ─┴→ [ESP32-C6 허브] ──집 Wi-Fi─┘
                       └─ Zigbee ─→ SmartThings 허브/Station ─→ SmartThings 앱
```

---

## 1. 준비물

| 항목 | 내용 |
|---|---|
| 보드 | ESP32-C6-DevKitC-1 (플래시 8MB). **J5 점퍼가 꽂혀 있어야** 칩에 전원이 들어갑니다 |
| USB | 보드의 **UART 포트**(CP210x)에 연결합니다. 펌웨어 쓰기·로그·진단 모두 이 포트를 씁니다 |
| PC | 펌웨어 쓰기: `esptool`(Python) 또는 Chrome/Edge 브라우저. 직접 빌드는 [부록 A](#부록-a-펌웨어-직접-빌드하기) |
| SmartThings | Zigbee 를 받는 허브(스마트 홈 허브 또는 Station), LYNNAI Edge 드라이버 |
| 선택: 센서 | 온습도계 SHT31 = SDA GPIO6, SCL GPIO7(주소 0x44) · 가스 계량기 리드 스위치 = GPIO10 ↔ GND |

---

## 2. 펌웨어 쓰기

빌드된 이미지를 GitHub **Releases** 에서 받아 씁니다. 이미지는 두 가지입니다.

| 파일 | 쓰는 주소 | 언제 |
|---|---|---|
| `lynnai-hub-esp32c6-<버전>-full.bin` | `0x0` | **처음 설치**. 부트로더·파티션 표·앱이 다 들어 있음. 허브 설정(Wi-Fi·관리자 암호 등)이 지워짐 |
| `lynnai-hub-esp32c6-<버전>-app.bin` | `0x20000` | **업데이트**. 앱만 바꿈. 설정·Zigbee 가입·허브 인증서 유지 |

### 2.1 USB 연결

- 보드의 **UART 포트**를 PC 에 연결합니다.
- 포트 이름: Linux `/dev/ttyUSB0`, macOS `/dev/cu.usbserial-*`, Windows `COM3` 등
  (Windows 는 **장치 관리자 → 포트(COM & LPT)** 의 *Silicon Labs CP210x USB to UART Bridge (COMx)*.
  안 보이면 Silicon Labs 의 CP210x VCP 드라이버를 설치).
- **WSL**: Windows 에서 `usbipd list` 로 CP210x 의 BUSID 를 찾고 `usbipd attach --wsl --busid <BUSID>`.
  보드를 다시 꽂을 때마다 다시 붙여야 합니다.

### 2.2 esptool 로 쓰기 (Windows · macOS · Linux)

```sh
pip install esptool                                   # 한 번
python -m esptool --chip esp32c6 -p COM3 write_flash 0x0 lynnai-hub-esp32c6-<버전>-full.bin       # 처음 설치
python -m esptool --chip esp32c6 -p COM3 write_flash 0x20000 lynnai-hub-esp32c6-<버전>-app.bin    # 업데이트
```

포트(`COM3`)는 2.1 에서 찾은 이름으로 바꿉니다. 쓰기가 끝나면 보드가 저절로 다시 켜집니다.

### 2.3 브라우저로 쓰기 (설치 없이)

1. Chrome 또는 Edge 에서 Espressif 의 웹 플래셔(<https://espressif.github.io/esptool-js/>)를 엽니다.
2. **Connect** → 보드 포트를 고릅니다.
3. Flash Address 에 `0x0`(처음 설치) 또는 `0x20000`(업데이트)을 넣고 해당 파일을 고른 뒤 **Program**.
4. 끝나면 보드의 RESET 버튼을 누르거나 USB 를 뺐다 꽂습니다.

처음 설치한 허브는 첫 부팅 때 기기 접속용 인증서를 만듭니다(몇 초). 그 뒤 [3장](#3-첫-설정)으로 갑니다.

---

## 3. 첫 설정

1. 전원을 넣으면 허브가 개방형 AP **`LYNNAI-SETUP-<기기ID>`** 를 띄웁니다.
2. 휴대폰으로 그 AP 에 붙으면 설정 페이지가 자동으로 열립니다. 안 열리면 브라우저에서 `http://198.18.0.1/`.
3. 입력: 집 Wi-Fi 이름·암호, 허브 AP 이름·암호(기기 등록용, 8자 이상), 관리자 암호(8자 이상).
4. 저장하면 허브가 집 Wi-Fi 에 붙고 WPA2 AP **`LYNNAI-HUB-<기기ID>`** 를 띄웁니다.
5. 공유기에서 허브(MAC 은 USB 진단 `diag` 에 나옴)에 **고정 IP 를 할당**해 두면 관리 화면 주소가 바뀌지 않습니다.

---

## 4. 연결

### 4.1 허브 버튼

보드의 BOOT 버튼을 씁니다.

| 누르기 | 동작 |
|---|---|
| **짧게** (3초 미만) | 스마트홈 허브와 붙기: Zigbee 가입 시도(3분) + LAN 드라이버 페어링 창(3분) |
| **길게** (3초 이상) | 새 기기 Wi-Fi 페어링: 보일러/온도 조절기를 허브 AP 에 등록 |

### 4.2 SmartThings (Zigbee)

1. LYNNAI Edge 드라이버를 SmartThings 허브에 설치합니다. 초대 링크
   <https://bestow-regional.api.smartthings.com/invite/Qrjp9gnaJbMq> 를 열어 SmartThings 계정으로 로그인 →
   **등록(Enroll)** → **사용 가능한 드라이버**에서 *LYNNAI Hub (Zigbee)* 를 허브에 **설치**합니다
   (LAN 통로를 쓸 때만 *LYNNAI Hub (LAN)* 도). 개발자는 [8장](#8-edge-드라이버-배포-개발자)처럼 CLI 로 설치해도 됩니다.
2. SmartThings 앱 → **기기 추가 → 주변 기기 검색**을 켭니다.
3. 허브 버튼을 **짧게** 누릅니다. 네트워크에서 `http://<허브IP>/api/zigbee/join` 을 열어도 됩니다.
4. 앱에 **LYNNAI Hub** 가 나타납니다. 보일러·방·센서 기기는 해당 기기가 처음 상태를 보내면 자동으로 생깁니다.
   이름은 SmartThings 앱에서 바꿉니다.

가입하는 동안 허브는 잠깐 Wi-Fi 를 멈춥니다(2.4GHz 라디오를 Zigbee 와 나눠 쓰므로). 끝나면 다시 붙습니다.

### 4.3 린나이 기기 (Wi-Fi 페어링)

1. 보일러 조절기(또는 온도 조절기)의 Wi-Fi 모듈을 **Wi-Fi 설정 모드**로 둡니다(모듈의 리셋 버튼).
2. 허브 버튼을 **길게(3초)** 누릅니다. 허브가 약 20초 동안 기기를 찾으므로 1과 2의 순서는 바뀌어도 됩니다.
3. LED: 파랑 = 기기 찾는 중, 노랑 = 허브 AP 정보 전송 중, 초록 = 성공, 빨강 = 실패.
4. 성공하면 기기가 허브 AP 에 붙고 관리 화면의 **연결된 기기**에 나타납니다.

### 4.4 관리 화면

`http://<허브IP>/admin` (관리자 암호, 30분 세션)

| 구역 | 내용 |
|---|---|
| 동작 모드 | **프록시**(린나이 클라우드로 중계, 린나이 앱 그대로) / **단독**(클라우드 없이 허브가 직접, 린나이 앱은 안 됨). 바꾸면 기기가 다시 접속할 때 적용([4.6](#46-단독-모드)) |
| 연결 방식 | Zigbee / LAN 중 사용할 통로 |
| 연결된 기기 | 기기 목록·연결 상태, ✏️ 이름(비우고 저장하면 기기 삭제), 온도 조절기의 보일러 연결, 기기·방별 노출 |
| 노출 설정 | 기기 종류별로 SmartThings 에 보일 컨트롤 |
| 서버 연동 | API 토큰(`/api/v1/state`, `/metrics` 를 쓰는 기록기용) |
| 플러그인 | 온습도계·가스 계량기 켜기, 핀, 노출, 계량기 숫자 맞추기, 🗑️ 삭제 |

### 4.5 LAN 드라이버 (선택)

관리 화면 **연결 방식**에서 LAN 을 켜고 LAN Edge 드라이버를 설치합니다. 새 드라이버가 처음 붙을 때 허브 버튼을
**짧게** 눌러 3분 페어링 창을 엽니다. 이미 페어링된 드라이버는 다시 누를 필요가 없습니다.

### 4.6 단독 모드

관리 화면의 **동작 모드 → 단독**(또는 USB 시리얼 `mode standalone`)으로 바꾸면 허브가 린나이 클라우드에 붙지 않고
기기의 MQTT 상대가 됩니다. 인터넷이 끊겨도 SmartThings 로 상태를 보고 제어할 수 있지만 린나이 앱에서는 기기가
오프라인으로 보입니다. 클라우드가 하던 일은 허브가 대신합니다.

- 기기가 접속해 구독하면 곧바로, 그 뒤 30초마다 상태를 요청합니다.
- 기기가 약 1시간마다 묻는 현재 시각에 한국 시각으로 답합니다(예약 기능용). 허브 시계가 아직 안 맞았으면(인터넷이
  한 번도 안 된 경우) 답하지 않습니다.
- 기기가 접속 정보(`/key/server`)를 물으면 프록시 때 받아 둔 그 기기의 답을, 없으면 허브가 만든 답을 줍니다.

프록시로 등록해 쓰던 기기는 그대로 단독으로 옮겨 붙습니다. 되돌릴 때는 **프록시**를 고르면 됩니다.

---

## 5. 초기화

USB 를 연결하고 `esptool`([2.2](#22-esptool-로-쓰기-windows--macos--linux))로 실행합니다. 주소는 `partitions.csv` 기준입니다.

| 무엇을 | 명령 | 결과 |
|---|---|---|
| **설정만** | `python -m esptool --chip esp32c6 -p <포트> erase_region 0x9000 0x6000` | Wi-Fi·관리자 암호·플러그인·노출·이름·동작 모드·허브 인증서가 지워지고 첫 설정 AP 로 돌아감. 기기 ID 도 새로 뽑힘. **Zigbee 가입은 유지** |
| **Zigbee 가입만** | `python -m esptool --chip esp32c6 -p <포트> erase_region 0x625000 0x10000` | 허브가 Zigbee 초기 상태가 됨. SmartThings 에서 옛 LYNNAI Hub 를 지우고 [4.2](#42-smartthings-zigbee)대로 다시 추가 |
| **전부** | `python -m esptool --chip esp32c6 -p <포트> erase_flash` 후 처음 설치 이미지 쓰기([2장](#2-펌웨어-쓰기)) | 공장 출하 상태. 새 기기 ID 가 뽑힘 |

지운 뒤 보드를 다시 켜면 적용됩니다. 기기 ID 가 바뀌면 허브 AP 의 기본 이름(`LYNNAI-HUB-<기기ID>`)도 바뀝니다.
첫 설정에서 **예전과 같은 허브 AP 이름·암호**를 넣으면 린나이 기기가 그대로 다시 붙고,
다르게 넣으면 [4.3](#43-린나이-기기-wi-fi-페어링)대로 기기를 다시 등록합니다.

---

## 6. 고장 분석

### 6.1 LED 오류 코드

빨강이 **N번 깜빡**인 뒤 2초 쉽니다. 여러 개면 돌아가며 보여 줍니다.

| 깜빡임 | 뜻 | 먼저 볼 것 |
|---|---|---|
| 1 | 집 Wi-Fi 2분 넘게 끊김 | 공유기 전원, Wi-Fi 이름·암호 변경 여부, 허브 위치(신호 약함) |
| 2 | Zigbee 미가입(부팅 90초 뒤에도) | SmartThings 에서 기기 추가 켜고 버튼 짧게 |
| 3 | 린나이 기기 링크 5분 넘게 끊김 | 기기 전원, 기기가 허브 AP 에 붙었는지(`diag` 의 `ap_clients`), 필요하면 버튼 길게로 다시 등록 |
| 4 | 저장 데이터 오류(재부팅 전까지 유지) | 관리 화면 맨 위 경고에 나온 설정을 다시 저장한 뒤 재부팅 |

진행 표시(파랑·노랑)가 켜져 있는 동안에는 오류 코드 대신 진행 색이 보입니다.

### 6.2 USB 시리얼 진단

네트워크로 허브에 닿지 않을 때 쓰는 방법입니다. UART 포트에 연결하고 115200bps 로 `diag` 한 줄을 보냅니다.

```sh
python3 tools/hub_doctor.py --serial /dev/ttyUSB0      # diag 를 받아 해석하고 파일로 저장
```

`#DIAG-BEGIN … #DIAG-END` 에 펌웨어 버전, 재시작 사유, 메모리, LED 코드, 집 Wi-Fi(SSID·IP·신호),
허브 AP 에 붙은 기기, Zigbee 상태, 린나이 기기 링크, 저장 오류, NVS 사용량, 최근 이벤트 40개가 나옵니다.
**비밀값(Wi-Fi·AP 암호, 관리자 암호, API 토큰, 키)은 나오지 않으므로** 결과 파일을 그대로 공유해도 됩니다.
다른 명령: `help`, `led`, `mode`(지금 동작 모드), `mode proxy` / `mode standalone`(동작 모드 바꾸기 — 관리 화면에
들어갈 수 없을 때. USB 를 꽂을 수 있으면 관리자와 같은 권한으로 본다).

시리얼 포트를 열 때 보드가 리셋되거나 버튼이 **짧게** 눌린 것으로 처리될 수 있습니다(USB-UART 의 DTR/RTS 가
리셋·BOOT 에 연결됨). Zigbee 미가입 상태라면 이때 가입 시도가 시작되어 Wi-Fi 가 몇 분 멈출 수 있습니다.
전체 로그는 아무 시리얼 터미널(115200bps)로 보거나, 빌드 환경이 있으면 `idf.py -p <포트> monitor` 로 봅니다(보드가 리셋됨).

### 6.3 네트워크 진단

```sh
python3 tools/hub_doctor.py                    # 허브 API 로 점검(기본 192.168.0.50, --host 로 변경)
python3 tools/hub_doctor.py --token <토큰>     # 관리 화면에서 API 토큰을 켠 경우
python3 tools/hub_doctor.py --st               # SmartThings CLI 로 ST 기기 상태도 점검
```

직접 볼 수 있는 주소:

| 주소 | 내용 |
|---|---|
| `/api/stats` | 실제 Zigbee 가입 여부(`zb_joined_now`), 초기 상태 여부, 메모리, LED 코드, 저장 오류, 이벤트 횟수 |
| `/api/events?since=N` | 최근 이벤트(접속·끊김·TLS·가입 실패 등, 부팅 후 ms 기준) |
| `/api/v1/state` | 보일러·방 상태(해독값 + 원본), 온습도, 가스 |

### 6.4 자주 겪은 문제

| 증상 | 원인 | 해결 |
|---|---|---|
| SmartThings 기기가 전부 오프라인, LED 2번 | 허브 Zigbee 가입 정보가 없음(부팅 로그 `factory-reset mode`) | 기기 추가 켜고 버튼 짧게. ST 의 ONLINE 표시는 늦게 바뀔 수 있음 |
| Zigbee 가입 실패가 `0x03` 으로 반복 | 열린 Zigbee 망이 없음 | SmartThings 앱에서 **기기 추가 → 주변 기기 검색**이 켜져 있는지 |
| 허브가 집 LAN 에서 사라짐, 이벤트에 `sta_down` 사유 201 | Zigbee 스캔이 라디오를 오래 잡음 | 펌웨어가 재시도 간격을 늘려 처리. 계속되면 Zigbee 를 다시 가입 |
| 가상 '보일러 1' 이 생김 | 온도 조절기가 붙을 실제 보일러를 못 찾음 | 보일러 조절기 연결 확인. 붙으면 자동으로 사라짐. 관리 화면에서 연결 지정 가능 |
| 린나이 앱이 안 됨 | 단독 모드 | 프록시 모드로 바꾸면 됨(단독 모드에서는 정상) |
| 단독 모드에서 처음 등록한 기기가 프록시로 바꾼 뒤 안 붙음 | 단독 모드 등록 때 허브가 임시 접속 정보를 줌 | 버튼 길게로 그 기기를 다시 등록(프록시 모드에서) |
| 이벤트에 클라우드 TLS 실패 `-188`, 로그에 "클라우드 인증서 검증 실패" | 클라우드가 고정한 CA(ckstack)와 다른 인증서를 냄 — 가짜 서버이거나 린나이가 CA 를 바꿈 | 네트워크 경로 확인. 린나이가 바꾼 것이면 `cloud_ca.crt` 를 새 CA 로 바꾼 펌웨어로 업데이트. 그동안은 단독 모드로 |
| 부팅 직후 클라우드 TLS 실패(err -308) 몇 번 | 부팅 직후 연결이 몰림 | 곧 재접속되면 정상 |
| 저장 데이터 오류(LED 4번) | 저장 중 전원 끊김 등으로 값 손상 | 경고에 나온 설정을 다시 저장 후 재부팅. 반복되면 보드 점검 |
| 메모리 최저치가 낮음(`heap_min`) | 여러 기기가 동시에 TLS 연결 | 일시적이면 무시. 재시작이 반복되면 `diag` 의 재시작 사유 확인 |
| 재시작 사유가 패닉·워치독 | 펌웨어 오류 | 시리얼 로그(또는 `idf.py monitor`)에서 Guru Meditation 백트레이스를 받아 보고 |

---

## 7. 보안 메모

- eFuse 를 쓰지 않으므로 플래시·NVS 암호화가 없습니다. USB 로 플래시를 통째로 읽으면 집 Wi-Fi 암호,
  허브 AP 암호, API 토큰, 허브 인증서 키가 평문으로 보입니다. 관리자 암호는 PBKDF2 해시로만 저장합니다.
- 기기는 허브 인증서를 검증하지 않습니다(그래서 허브가 기기를 받을 수 있음).
- 허브는 린나이 클라우드 인증서를 **사설 CA "ckstack"(펌웨어의 `main/cloud_ca.crt`, 2123년까지)으로 검증**합니다.
  서버 인증서 이름이 `dev2.ckpush.com` 이라 호스트 이름은 보지 않고 서명 체인만 확인합니다. 허브 시계가 맞기 전
  (부팅 직후)에는 날짜 검사만 건너뜁니다. NAS 프록시도 같은 CA 로 검증합니다(`UPSTREAM_CA`).
- `/key/server` 응답은 기기별로, 정상(성공 코드) 응답만 저장합니다. 단독 모드에서 저장된 응답이 없으면 허브가
  임시 접속 정보를 만들어 줍니다.
- 클라우드 주소는 허브가 집 Wi-Fi 쪽 DNS 로 직접 조회합니다(허브 자체 DNS 는 기기를 허브로 돌리기 위해
  `rkiot.net` 을 허브로 답합니다).

---

## 8. Edge 드라이버 배포 (개발자)

```sh
smartthings edge:drivers:package edge-driver --channel <채널ID>       # 공개 채널 "LYNNAI Hub" = 9231d747-aeb6-45da-8436-22ac6161f531
smartthings edge:drivers:install <드라이버ID> --hub <허브 전체 UUID> --channel <채널ID>
smartthings edge:drivers:logcat <드라이버ID> --hub-address <허브IP>     # 드라이버 로그
cd edge-driver/test && python3 run_sim.py                                # 하드웨어 없이 드라이버 시험(lupa 필요)
```

- 허브 ID 는 전체 UUID 를 써야 합니다(앞부분만 쓰면 400 오류).
- 이미 등록된 기기는 새 드라이버 설치만으로 바뀌지 않으면 `edge:drivers:switch` 로 옮깁니다.

---

## 부록 A. 펌웨어 직접 빌드하기

릴리즈 이미지 대신 소스에서 빌드할 때만 필요합니다. 펌웨어에는 비밀값이 들어가지 않습니다(기기 접속용 인증서는
허브가 첫 부팅 때 직접 만들고, 클라우드 CA `main/cloud_ca.crt` 는 공개 정보).

### A.1 Linux · macOS · WSL

```sh
# ESP-IDF v5.5.6 설치(한 번) — https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/
. ~/esp/esp-idf/export.sh          # 터미널마다
cd firmware/esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash        # 처음: 부트로더·파티션 표·앱 전부
idf.py -p /dev/ttyUSB0 app-flash    # 이후: 앱만(설정·Zigbee 가입 정보 유지)
idf.py -p /dev/ttyUSB0 monitor      # 로그(보드가 리셋됨, 끝내기 Ctrl+])
```

- 처음 빌드에서 "wolfssl 매니지드 컴포넌트가 없습니다"가 나오면 `idf.py reconfigure` 를 한 번 더 하고 빌드합니다.
  wolfSSL 설정 원본은 `firmware/esp32/wolfssl/user_settings.h` 입니다.
- `main/` 에 새 `.c` 파일을 추가하면 `idf.py reconfigure` 가 필요합니다.
- 파티션 표(`partitions.csv`)를 바꿨다면 `flash` 로 전부 다시 쓰고, 필요하면 [초기화](#5-초기화)를 함께 합니다.

### A.2 Windows

WSL 없이 Windows 에서 바로 빌드하는 방법입니다.

1. **ESP-IDF 설치**: Espressif 의 *ESP-IDF Tools Installer for Windows*(온라인 또는 오프라인)를 받아 실행하고
   버전 **v5.5.6** 을 고릅니다. Python·Git·툴체인이 같이 깔리고, 시작 메뉴에 **ESP-IDF 5.5 PowerShell** /
   **ESP-IDF 5.5 CMD** 바로가기가 생깁니다. 설치 경로와 레포 경로는 **짧고 공백·한글이 없는 곳**
   (예: `C:\esp`, `C:\work\lynnai-hub`)에 둡니다. 경로가 길거나 공백이 있으면 빌드가 실패할 수 있습니다.
   VS Code 를 쓴다면 *ESP-IDF* 확장에서 같은 버전을 설치해도 됩니다.
2. **USB 포트**: [2.1](#21-usb-연결)처럼 COM 포트를 찾습니다.
3. **빌드·쓰기**: 시작 메뉴의 **ESP-IDF 5.5 PowerShell** 을 열고(일반 PowerShell 은 `idf.py` 를 모름)

   ```powershell
   cd C:\work\lynnai-hub\firmware\esp32
   idf.py build
   idf.py -p COM3 flash          # 처음
   idf.py -p COM3 app-flash      # 이후
   idf.py -p COM3 monitor        # 로그 보기(보드가 리셋됨, 끝내기 Ctrl+])
   ```

4. **진단 스크립트**: `pip install pyserial` 후 `python tools\hub_doctor.py --serial COM3`.
5. **Edge 드라이버를 Windows 에서 패키징**한다면: `edge-driver-lan/` 이 `edge-driver/` 의 파일을 심볼릭 링크로
   가리킵니다. Windows 개발자 모드를 켜고 `git config --global core.symlinks true` 를 한 뒤 클론해야 링크가
   살아 있습니다. 아니면 WSL·Linux 에서 패키징합니다.

### A.3 릴리즈 이미지 만들기

빌드한 뒤 `firmware/esp32` 에서:

```sh
idf.py merge-bin -o lynnai-hub-esp32c6-<버전>-full.bin     # 처음 설치용(0x0): build/ 안에 생김
cp build/lynnai_hub.bin lynnai-hub-esp32c6-<버전>-app.bin  # 업데이트용(0x20000)
```
