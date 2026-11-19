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
| PC | ESP-IDF **v5.5.6**. Linux, macOS, Windows(WSL) |
| SmartThings | Zigbee 를 받는 허브(스마트 홈 허브 또는 Station), LYNNAI Edge 드라이버 |
| 선택: 센서 | 온습도계 SHT31 = SDA GPIO6, SCL GPIO7(주소 0x44) · 가스 계량기 리드 스위치 = GPIO10 ↔ GND |

---

## 2. 펌웨어 쓰기

### 2.1 개발 환경

```sh
# ESP-IDF v5.5.6 설치(한 번) — https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/get-started/
. ~/esp/esp-idf/export.sh          # 터미널마다
cd firmware/esp32
```

### 2.2 기기 접속용 인증서(srv.pem)

빌드에는 `firmware/esp32/main/srv.pem`(인증서 + 개인키)이 필요합니다. 레포에는 넣지 않습니다(`.gitignore`).
허브는 첫 부팅 60초 뒤 자기 인증서를 새로 만들어 쓰므로, 이 파일은 그 전까지만 쓰입니다.
보일러는 인증서를 검증하지 않으므로 자체 서명 인증서면 됩니다.

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -sha256 \
  -keyout /tmp/k.pem -out /tmp/c.pem -subj "/C=KR/O=rkiot/CN=rkiot.net" \
  -addext "subjectAltName=DNS:rkiot.net,DNS:*.rkiot.net,DNS:www.rkiot.net"
cat /tmp/c.pem /tmp/k.pem > main/srv.pem && rm /tmp/k.pem /tmp/c.pem
```

### 2.3 빌드와 쓰기

```sh
idf.py build
idf.py -p /dev/ttyUSB0 flash        # 처음: 부트로더·파티션 표·앱 전부
idf.py -p /dev/ttyUSB0 app-flash    # 이후: 앱만(설정·Zigbee 가입 정보 유지)
```

- 처음 빌드에서 "wolfssl 매니지드 컴포넌트가 없습니다"가 나오면 `idf.py reconfigure` 를 한 번 더 하고 빌드합니다.
  wolfSSL 설정 원본은 `firmware/esp32/wolfssl/user_settings.h` 입니다.
- `main/` 에 새 `.c` 파일을 추가하면 `idf.py reconfigure` 가 필요합니다.
- 포트 이름: Linux `/dev/ttyUSB0`, macOS `/dev/cu.usbserial-*`, Windows `COM3` 등.
- **WSL**: Windows 에서 `usbipd list` 로 CP210x 의 BUSID 를 찾고 `usbipd attach --wsl --busid <BUSID>`.
  보드를 다시 꽂을 때마다 다시 붙여야 합니다.

`app-flash` 는 설정을 지우지 않습니다. 파티션 표(`partitions.csv`)를 바꿨다면 `flash` 로 전부 다시 쓰고,
필요하면 아래 [초기화](#5-초기화)를 함께 합니다.

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

1. LYNNAI Edge 드라이버를 SmartThings 허브에 설치합니다(초대 링크 또는 [8장](#8-edge-드라이버-배포-개발자)).
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
| 동작 모드 | **프록시**(린나이 클라우드로 중계, 린나이 앱 그대로) / **단독**(클라우드 없이 허브가 직접, 린나이 앱은 안 됨) |
| 연결 방식 | Zigbee / LAN 중 사용할 통로 |
| 연결된 기기 | 기기 목록·연결 상태, ✏️ 이름(비우고 저장하면 기기 삭제), 온도 조절기의 보일러 연결, 기기·방별 노출 |
| 노출 설정 | 기기 종류별로 SmartThings 에 보일 컨트롤 |
| 서버 연동 | API 토큰(`/api/v1/state`, `/metrics` 를 쓰는 기록기용) |
| 플러그인 | 온습도계·가스 계량기 켜기, 핀, 노출, 계량기 숫자 맞추기, 🗑️ 삭제 |

### 4.5 LAN 드라이버 (선택)

관리 화면 **연결 방식**에서 LAN 을 켜고 LAN Edge 드라이버를 설치합니다. 새 드라이버가 처음 붙을 때 허브 버튼을
**짧게** 눌러 3분 페어링 창을 엽니다. 이미 페어링된 드라이버는 다시 누를 필요가 없습니다.

---

## 5. 초기화

USB 를 연결하고 ESP-IDF 환경에서 실행합니다. 주소는 `partitions.csv` 기준입니다.

| 무엇을 | 명령 | 결과 |
|---|---|---|
| **설정만** | `esptool.py --chip esp32c6 -p /dev/ttyUSB0 erase_region 0x9000 0x6000` | Wi-Fi·관리자 암호·플러그인·노출·이름·동작 모드·허브 인증서가 지워지고 첫 설정 AP 로 돌아감. 기기 ID 도 새로 뽑힘. **Zigbee 가입은 유지** |
| **Zigbee 가입만** | `esptool.py --chip esp32c6 -p /dev/ttyUSB0 erase_region 0x625000 0x10000` | 허브가 Zigbee 초기 상태가 됨. SmartThings 에서 옛 LYNNAI Hub 를 지우고 [4.2](#42-smartthings-zigbee)대로 다시 추가 |
| **전부** | `idf.py -p /dev/ttyUSB0 erase-flash` 후 `idf.py -p /dev/ttyUSB0 flash` | 공장 출하 상태. 새 기기 ID 가 뽑힘 |

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
다른 명령: `help`, `led`.

시리얼 포트를 열 때 보드가 리셋되거나 버튼이 **짧게** 눌린 것으로 처리될 수 있습니다(USB-UART 의 DTR/RTS 가
리셋·BOOT 에 연결됨). Zigbee 미가입 상태라면 이때 가입 시도가 시작되어 Wi-Fi 가 몇 분 멈출 수 있습니다.
전체 로그는 `idf.py -p /dev/ttyUSB0 monitor` 로 봅니다(보드가 리셋됨).

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
| 부팅 직후 클라우드 TLS 실패(err -308) 몇 번 | 부팅 직후 연결이 몰림 | 곧 재접속되면 정상 |
| 저장 데이터 오류(LED 4번) | 저장 중 전원 끊김 등으로 값 손상 | 경고에 나온 설정을 다시 저장 후 재부팅. 반복되면 보드 점검 |
| 메모리 최저치가 낮음(`heap_min`) | 여러 기기가 동시에 TLS 연결 | 일시적이면 무시. 재시작이 반복되면 `diag` 의 재시작 사유 확인 |
| 재시작 사유가 패닉·워치독 | 펌웨어 오류 | `idf.py monitor` 로 Guru Meditation 백트레이스를 받아 보고 |

---

## 7. 보안 메모

- eFuse 를 쓰지 않으므로 플래시·NVS 암호화가 없습니다. USB 로 플래시를 통째로 읽으면 집 Wi-Fi 암호,
  허브 AP 암호, API 토큰, 허브 인증서 키가 평문으로 보입니다. 관리자 암호는 PBKDF2 해시로만 저장합니다.
- 기기는 허브 인증서를 검증하지 않고, 허브도 린나이 클라우드 인증서를 검증하지 않습니다.
- 클라우드 주소는 허브가 집 Wi-Fi 쪽 DNS 로 직접 조회합니다(허브 자체 DNS 는 기기를 허브로 돌리기 위해
  `rkiot.net` 을 허브로 답합니다).

---

## 8. Edge 드라이버 배포 (개발자)

```sh
smartthings edge:drivers:package edge-driver --channel <채널ID>
smartthings edge:drivers:install <드라이버ID> --hub <허브 전체 UUID> --channel <채널ID>
smartthings edge:drivers:logcat <드라이버ID> --hub-address <허브IP>     # 드라이버 로그
cd edge-driver/test && python3 run_sim.py                                # 하드웨어 없이 드라이버 시험(lupa 필요)
```

- 허브 ID 는 전체 UUID 를 써야 합니다(앞부분만 쓰면 400 오류).
- 이미 등록된 기기는 새 드라이버 설치만으로 바뀌지 않으면 `edge:drivers:switch` 로 옮깁니다.
