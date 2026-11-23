# lynnai-nas (NAS 모드)

> 설치·운영 안내(사용자용)는 [docs/nas-hub.md](../../docs/nas-hub.md). 이 파일은 구조와 환경변수 설명이다.
> **쓰려면 집 네트워크(공유기 DHCP)의 DNS 서버를 NAS 로 바꿔야 한다** — 집 안 모든 기기에 적용된다.

ESP32 허브 없이 NAS(도커) 하나로 린나이 보일러·각방 조절기를 받는 모드.
**LAN 으로만 동작하는 허브**일 뿐, ESP32 허브와 동작이 같다(Wi-Fi AP·Zigbee·Matter·꽂는 센서만 빠짐).
로직은 레포의 [`standalone/`](../../standalone) 와 ESP32 펌웨어를 옮긴 것이고, 이 폴더가 그걸 조립한다.

## proxy 모드 (기본, ESP32 허브와 동일)

```
린나이 기기 ──DNS "rkiot.net?"─▶ NAS(:53)   등록된 기기면 NAS 주소로 답(나머지는 공유기로 넘기고 후보로 적음)
           ──HTTP /key/server─▶ NAS(:80)    진짜 클라우드로 다시 물어 전달(정상 응답은 기기별로 저장)
           ──MQTT/TLS────────▶ NAS(:8883)  기기 TLS 종단 → 진짜 린나이 클라우드로 중계(앱 그대로)
                                            지나가는 상태를 읽고 SmartThings 명령을 끼워 넣음
SmartThings ◀─SSDP + TCP──────  NAS(:8484)  LAN Edge 드라이버(edge-driver-lan)가 그대로 붙음
서버 기록   ◀─/api/v1/state·/metrics─ NAS(:80)  lynnai-hub-watch·Prometheus (ESP32 와 같은 형식)
브라우저   ──/devices────────▶ NAS(:80)    후보로 보인 기기를 승인(등록)·삭제
```

| 파일 | ESP32 대응 | 역할 |
|---|---|---|
| `hub_proxy.py` | `hub_proxy.c` (원본 Pi `bridge.py`) | :8883 기기 TLS 종단 → 클라우드 재연결 중계(proxy) 또는 직접 응답(standalone) |
| `upstream.py` | `hub_proxy.c`(`resolve_up`) | 클라우드 주소: `UPSTREAM_IP` 또는 `www.rkiot.net` DNS 조회(답의 주소 전부, TTL 만큼), 연결 |
| `hub_rinnai.py` | `hub_rinnai.c` + `hub_uplink.c` | 상태 스눕(EPC70)·명령 조립(J07 `sum`=rpc수)·내보내기 |
| `hub_lan.py` | `hub_lan.c` | SSDP 찾기 + TCP 8484 통로(hello 토큰, 페어링 창, ping) |
| `hub_api.py` | `hub_api.c`, `hub_proxy.c`(`/key/server`) | `/api/v1/state`·`/metrics`, 부트스트랩(기기별 저장·단독 응답), 기기 등록 페이지 `/devices`·`/api/v1/devices` |
| `hub_devices.py` | 버튼 길게(기기 등록 창) | 기기 등록부: 후보(메모리) → 승인(`data/devices.json`), ARP 표로 MAC 판정, `DEVICE_PAIR` 등록 창 |
| `hub_bootstrap.py` | `hub_proxy.c`(`boot_fetch`) | 부트스트랩을 진짜 클라우드에 다시 묻기 |
| `nas_hub.py` | `hub_main.c`(`app_main`) | 위를 공용 코어 하나로 조립(두 모드의 진입점) |
| `standalone/dns_hijack.py` | `hub_dns.c` | 등록된 기기의 rkiot.net(과 하위 이름)만 가로챔(등록부를 질의마다 물음 — `is_src_ok`), 등록 안 된 출발지가 물으면 후보로(`on_unregistered`) |

### 린나이 서버가 안 될 때 (자동 단독 동작)

ESP32 허브와 같은 규칙이다. proxy 모드에서 클라우드 쪽이 두 번 연달아 실패하면 그동안만 standalone 처럼 NAS 가 기기를
직접 받고, 1분마다 서버에 보안 연결을 해 봐서 두 번 연달아 되면 proxy 로 돌아간다(다시 실패하면 간격을 10분까지 늘린다).
"성공"은 서버가 기기의 로그인을 받아 준 것이다 — 연결은 되는데 로그인을 거절하거나 답이 없는 서버도 실패로 센다.
스마트홈의 허브 기기에는 모드(설정)와 상태(`단독 모드 (18:25부터 린나이 서버 연결 안 됨)`)가 나오고, `/api/v1/state` 의
`mode` 에도 같은 내용이 있다. 끄려면 `AUTO_FALLBACK=0`. 스마트홈에서 자동 단독 동작은 켜고 끌 수 있지만(다시 시작하면
환경변수 값으로), 모드 자체(proxy/standalone)는 `MODE` 로만 정한다.

서버 주소는 DNS 답만 따른다: 답에 든 주소들을 그 TTL 동안만 쓰고(30초~1시간), 지나면 다시 묻는다. `UPSTREAM_IP` 를
주면 그 주소만 쓴다.

시험: `python3 test_fallback.py` — 가짜 기기와 가짜 서버로 위 동작을 돌려 본다(openssl 필요).

## standalone 모드 (클라우드 없이 NAS 가 직접, ESP32 단독 모드와 동일)

`MODE=standalone` 이면 같은 `nas_hub.py` 가 클라우드에 붙지 않고 기기의 MQTT 상대가 된다.
린나이 앱으로는 보이지 않지만 인터넷이 끊겨도 SmartThings(LAN 드라이버)·API 는 그대로 된다.
클라우드가 하던 일은 허브가 대신한다(MITM 캡처로 확인한 것):

- CONNECT/SUBSCRIBE/PUBLISH/PING 에 CONNACK·SUBACK·PUBACK·PINGRESP 로 답한다(아이디·암호는 따지지 않음).
- 기기가 `<base>/get` 을 구독하면 곧바로 상태 요청(`fa`)을 보낸다(30초마다 보내는 것은 두 모드 공통, ESP32 와 같음).
- 기기의 시간 요청(`<base>/stg`, 약 1시간마다)에 `<base>/set` 의 `72` = 현재 한국 시각(`YYYYMMDDTHHMMSS`)으로 답한다.
- `/key/server` 는 늘 허브가 만든 답(임시 아이디·암호)을 준다. 프록시로 쓰던 때 저장한 클라우드 계정은 내주지 않는다.

프록시로 등록했던 기기도 그대로 단독으로 옮겨 붙는다(단독 브로커는 아이디·암호를 따지지 않는다). 단독에서는 어느 기기든
임시 아이디·암호를 받으므로, 단독에서 처음 등록한 기기는 나중에 프록시로 바꾸면 다시 등록이 필요할 수 있다.
TLS 관문만 시험하려면 `GATE_TEST=1`(옛 `server_pi3.py`).

## 쓰기

1. 공유기에서 NAS 주소를 고정한다(보일러·각방 조절기 IP 는 고정하지 않아도 된다 — 등록은 MAC 으로 한다, 아래).
2. 공유기 **DHCP 의 DNS 서버**를 `1차: NAS, 2차: 공유기`로.
3. `.env` 를 만든다:
   ```
   NAS_IP=192.168.0.10            # 이 NAS 의 LAN 주소
   API_TOKEN=...                  # /api·/metrics 토큰이자 기기 승인 토큰, 16자 이상. 없으면 /devices 에서 승인을 못 한다
   # DEVICE_PAIR=1                # (선택) 승인 대신: 시작 후 600초 동안 보이는 후보를 바로 등록(아래)
   # RINNAI_IPS=192.168.0.20,192.168.0.21   # (선택) IP 로 고정 등록(공유기에서 그 IP 를 고정해 둘 것)
   # FORWARD=none                 # (선택) 완전 독립: 다른 도메인도 안 넘김
   ```
4. 레포 루트에서:
   ```sh
   docker compose -f server/lynnai-nas/docker-compose.yml up -d --build
   ```
5. 보일러·각방 전원을 껐다 켠다. 기기가 새 DNS(NAS)에 rkiot.net 을 묻는다 — 아직 등록 전이라 NAS 는 그 질의를 건드리지 않고
   (기기는 평소대로 린나이 클라우드로 간다) 그 기기를 **후보**로 적어 둔다.
6. 브라우저로 `http://<NAS 주소>/devices` 를 열어 토큰을 넣고, 후보에서 내 보일러·각방 조절기를 **승인**한다
   (MAC 이 `34:5f:45` 로 시작하면 "린나이 모듈로 보임"이라고 표시한다).
7. 승인한 기기 전원을 **한 번 더** 껐다 켠다. 기기는 DNS 를 다시 물을 때부터 NAS 로 온다(승인만 해서는 바로 안 옮겨 온다).
8. `.env` 에 `LAN_PAIR=1` 을 넣고 다시 띄운다(`up -d`). 시작 후 600초 동안만 새 LAN 드라이버를 등록한다
   (첫 설치도 같음 — 집 LAN 의 아무 기기나 먼저 붙어 등록되지 않게. ESP32 의 버튼 페어링 창과 같은 뜻).
9. 그 사이 SmartThings 앱에서 "기기 추가 → 주변 검색" → `edge-driver-lan`(LYNNAI Hub (LAN))이 NAS 를 찾아 붙는다.
   등록이 끝나면 `.env` 에서 `LAN_PAIR` 를 지우고 다시 띄운다(켜 두면 재시작마다 600초 열림).
   등록 안 된 드라이버는 `denied` 를 받는다(1초 늦게). 토큰은 `data/lan_tokens.json`(최대 4개, 넘으면 오래된 것부터).

### 기기 등록: 후보 → 승인

**등록된 기기** = 승인한 기기(`data/devices.json`) + `RINNAI_IPS` 에 적은 IP. 등록된 기기만 린나이 기기로 본다.
그 밖의 출발지는 `:8883` 에서 TLS 를 종단하지 않고(프록시: 진짜 클라우드로 그대로 넘김 / 단독: 끊음),
`/key/server` 는 403, DNS 질의는 건드리지 않고 상위로 넘긴다.

- **후보**: 등록 안 된 출발지가 NAS 의 DNS 에 rkiot.net(이나 그 하위 이름)을 물으면 후보 표에 적힌다(질의는 평소대로 상위로
  넘긴다). `Ck-Mac` 헤더를 달고 `/key/server` 를 부른 등록 안 된 출발지도 후보가 된다(답은 그대로 403).
  후보는 IP, MAC(모를 수 있음), 처음·마지막 본 시각, 횟수를 담고 **메모리에만** 있다(최대 16개, 넘으면 가장 오래 전에
  본 것부터 버림. 다시 시작하면 비워진다).
- **승인**: `http://<NAS 주소>/devices` 에서 토큰을 넣고 후보 줄의 "승인"을 누른다. 등록은 최대 8대, 같은 페이지에서 삭제한다.
  API 로도 된다:
  ```sh
  curl -H "Authorization: Bearer $API_TOKEN" http://NAS/api/v1/devices                               # 등록·고정·후보 목록
  curl -H "Authorization: Bearer $API_TOKEN" -d ip=192.168.0.20 http://NAS/api/v1/devices/approve    # 또는 mac=
  curl -H "Authorization: Bearer $API_TOKEN" -d mac=34:5f:45:00:00:01 http://NAS/api/v1/devices/remove
  ```
  승인·삭제는 **`API_TOKEN` 이 있어야만** 받는다. 토큰 없이 받으면 집 LAN 의 아무 기기나 자기를 승인할 수 있기 때문이다
  (토큰이 없으면 목록은 다른 `/api` 처럼 보이지만 승인·삭제는 403).
  이 페이지와 API 는 NAS 의 다른 API 처럼 **암호화 없는 HTTP** 다 — 토큰이 집 LAN 을 그대로 지나간다.
- **승인한 뒤**: 기기는 DNS 를 다시 물을 때부터 NAS 로 온다. 보통 기기 전원을 껐다 켜야 한다.
  삭제한 기기도 이미 붙어 있는 연결은 끊길 때까지 남고, 다시 DNS 를 물으면 후보로 돌아온다.
- **`DEVICE_PAIR=1`**(등록 창): 시작 후 600초 동안은 후보가 보이는 그 자리에서 등록한다(로그에 한 줄씩 남는다). 그 질의부터
  NAS 주소로 답하므로 기기 전원은 한 번만 껐다 켜면 된다. ESP32 허브의 등록 버튼(버튼 길게)·`LAN_PAIR` 와 같은 뜻이다.
  등록이 끝나면 `.env` 에서 지우고 다시 띄운다(켜 두면 재시작마다 600초 열림). 창이 열린 동안 rkiot.net 을 묻는 기기는
  **무엇이든** 등록되므로, 끝난 뒤 `/devices`(또는 `/api/v1/devices`)에서 내 기기만 있는지 보고 아닌 것은 삭제한다.
- **MAC 으로 판정**: NAS 는 질의·연결의 출발지 IP 를 호스트 ARP 표(`/proc/net/arp`, 30초 재사용)에서 찾아 그 MAC 이
  등록돼 있으면 등록된 기기로 본다. 그래서 공유기가 기기 IP 를 바꿔 줘도 다시 승인할 필요가 없다(바뀐 주소는 등록부에
  고쳐 적는다). ARP 표에 그 IP 가 없을 때는 마지막으로 본 IP 가 같은지로 본다. ARP 표를 읽으려면 **host 네트워크**여야 한다 —
  못 읽으면 경고 로그를 한 번 남기고 IP 로만 판정한다(이때는 기기 IP 를 공유기에서 고정해야 한다).
- **`RINNAI_IPS`**(선택): 여기 적은 IP 는 승인 없이 늘 등록된 기기다(IP 로만 판정하므로 공유기에서 고정할 것).
  비워 두면 고정 등록 없이 뜨고, 승인한 기기가 없으면 아무도 린나이 기기로 보지 않는다(시작 로그에 한 줄).
  IPv4 주소가 아닌 항목이 있으면 **뜨지 않는다**(오류 로그를 남기고 끝난다).

한계 — 있는 그대로:

- MAC 도 IP 도 같은 LAN 에서는 꾸밀 수 있다. MAC 등록은 `RINNAI_IPS` 의 IP 목록과 **같은 수준의 믿음**이지 인증이 아니다.
  등록된 기기의 MAC·IP 를 흉내 내는 LAN 기기는 린나이 기기로 받아들여진다.
- 승인은 사람이 판단한다. NAS 는 "rkiot.net 을 물었다"는 것과 MAC 앞자리만 본다. 린나이 앱을 쓰는 휴대폰도 rkiot.net 을
  물으면 후보로 보일 수 있다(확인해 보지는 않았다) — 그런 것은 승인하지 않는다. `DEVICE_PAIR` 창에서는 그것도 등록된다.
- 후보 표는 16개라, LAN 의 기기가 출발지를 바꿔 가며 물으면 진짜 후보가 밀려날 수 있다(그러면 기기 전원을 다시 켠다).

굳이 모두 열려면 `RINNAI_IPS=any` 를 명시한다(시작할 때 경고 로그). 그러면 `:8883` 에 붙는 **집 LAN 의 아무 기기**나
린나이 기기로 받아들인다. 그 기기가 가짜 상태를 올리면 SmartThings 에 가짜 기기가 생기거나 실제 상태가 덮이고,
드라이버 자동화가 그 상태로 실제 난방을 켜고 끌 수 있다. `/key/server`(기기 계정이 든 부트스트랩)도 누구에게나 답하고,
DNS 도 모든 기기의 rkiot.net 질의를 가로챈다.

`network_mode: host` 를 쓰는 이유: `:53`/`:8883` 가 집 LAN 에 직접 보여야 하고, DNS 가 **질의한 기기의
진짜 IP**를 보고 호스트 ARP 표에서 그 MAC 을 찾아야 등록된 기기를 걸러낼 수 있기 때문이다(브리지 네트워크면 출발지가 다
도커 IP 로 보인다).

## 환경변수

| 변수 | 뜻 | 기본 |
|---|---|---|
| `NAS_IP` | 이 NAS 의 LAN 주소(DNS 가 답할 값) | (필수) |
| `RINNAI_IPS` | 고정 등록할 린나이 기기 IP(쉼표, IPv4) — 승인한 기기에 더해 늘 등록된 것으로 본다. 비우면 고정 등록 없음. IPv4 가 아닌 항목이 있으면 뜨지 않는다. `any` = 모든 출발지(경고) | 비움 |
| `DEVICE_PAIR` | `1` 이면 시작 후 600초 동안 보이는 후보를 바로 등록(등록 창). 등록 뒤 지울 것 | 꺼짐 |
| `ONLY_SRC` | DNS 가로채기 대상만 따로 좁힐 때(IP, 쉼표). 주면 승인한 기기도 이 목록에 있어야 가로챈다 | 등록된 기기 전부 |
| `HIJACK_NAMES` | 가로챌 도메인(쉼표, 그 이름과 하위 이름) | `rkiot.net` |
| `API_TOKEN` | `/api`·`/metrics` Bearer 토큰이자 `/devices` 의 승인 토큰. 16자 미만이면 API 를 막음. 비우면 기기 승인·삭제를 받지 않는다 | 비움(LAN 에 열림) |
| `LAN_PAIR` | `1` 이면 시작 후 600초 동안 새 LAN 드라이버 등록(첫 설치 포함). 등록 뒤 지울 것 | 꺼짐 |
| `MAX_CONN` | `:8883` 동시 연결 상한. 등록 안 된 출발지끼리 나눠 쓴다(출발지당 4). 등록된 기기는 이 상한과 따로 센다(출발지당 4) | `16` |
| `FORWARD` | 목록 밖 질의를 넘길 상위 DNS. `none`=안 넘김 | 기본 게이트웨이 |
| `MODE` | `proxy`(클라우드 중계) / `standalone`(클라우드 없이) | `proxy` |
| `AUTO_FALLBACK` | proxy 인데 린나이 서버가 안 되면 그동안 NAS 가 기기를 직접 받는다(자동 단독 동작). `0` 이면 끔 | `1` |
| `UPSTREAM_IP` | 진짜 클라우드 주소(이것만 쓴다). 비우면 `www.rkiot.net` 을 DNS 로 조회해 답에 든 주소들을 그 TTL 동안 쓴다 | 비움 |
| `GATE_TEST` | 값이 있으면 TLS 핸드셰이크 성사/거부만 확인 | 꺼짐 |
| `UPSTREAM_CA` | proxy 모드에서 클라우드 인증서를 검증할 CA(린나이 사설 CA "ckstack"), 컨테이너 안 경로(바꾸려면 `/data/…`). 파일이 없으면 클라우드에 붙지 않음 | `/app/lynnai-nas/cloud_ca.crt`(이 폴더) |
| `UPSTREAM_CN` | 클라우드 인증서에 있어야 하는 이름(CN) | `dev2.ckpush.com` |

> `:80` 은 동시 연결 32개(출발지당 8, 등록된 기기는 따로 센다)까지 받고, 요청(본문 포함)을 10초 안에 다 보내지 않으면 끊는다.

> 컨테이너는 root 로 돌지만 capability 는 `NET_BIND_SERVICE`(53·80 바인드)만 남기고 다 뺀다(`cap_drop: ALL`,
> `no-new-privileges`). 그래서 `./data` 는 root 소유여야 한다(직접 만들었다면 `sudo chown -R root: data`).
> `init: true` 로 `docker stop` 의 SIGTERM 이 바로 먹는다.
