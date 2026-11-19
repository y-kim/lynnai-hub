# lynnai-nas (NAS 모드)

ESP32 허브 없이 NAS(도커) 하나로 린나이 보일러·각방 조절기를 받는 모드.
**LAN 으로만 동작하는 허브**일 뿐, ESP32 허브와 동작이 같다(Wi-Fi AP·Zigbee·Matter·꽂는 센서만 빠짐).
로직은 레포의 [`standalone/`](../../standalone) 와 ESP32 펌웨어를 옮긴 것이고, 이 폴더가 그걸 조립한다.

## proxy 모드 (기본, ESP32 허브와 동일)

```
린나이 기기 ──DNS "rkiot.net?"─▶ NAS(:53)   등록된 기기면 NAS 주소로 답(나머지는 공유기로 넘김)
           ──HTTP /key/server─▶ NAS(:80)    진짜 클라우드로 다시 물어 전달(정상 응답은 기기별로 저장)
           ──MQTT/TLS────────▶ NAS(:8883)  기기 TLS 종단 → 진짜 린나이 클라우드로 중계(앱 그대로)
                                            지나가는 상태를 읽고 SmartThings 명령을 끼워 넣음
SmartThings ◀─SSDP + TCP──────  NAS(:8484)  LAN Edge 드라이버(edge-driver-lan)가 그대로 붙음
서버 기록   ◀─/api/v1/state·/metrics─ NAS(:80)  lynnai-hub-watch·Prometheus (ESP32 와 같은 형식)
```

| 파일 | ESP32 대응 | 역할 |
|---|---|---|
| `hub_proxy.py` | `hub_proxy.c` (원본 Pi `bridge.py`) | :8883 기기 TLS 종단 → 클라우드 재연결 중계(proxy) 또는 직접 응답(standalone) |
| `upstream.py` | `hub_proxy.c`(`resolve_up`) | 클라우드 주소: `UPSTREAM_IP` 또는 `www.rkiot.net` DNS 조회 |
| `hub_rinnai.py` | `hub_rinnai.c` + `hub_uplink.c` | 상태 스눕(EPC70)·명령 조립(J07 `sum`=rpc수)·내보내기 |
| `hub_lan.py` | `hub_lan.c` | SSDP 찾기 + TCP 8484 통로(hello 토큰, ping) |
| `hub_api.py` | `hub_api.c`, `hub_proxy.c`(`/key/server`) | `/api/v1/state`·`/metrics`, 부트스트랩(기기별 저장·단독 응답) |
| `hub_bootstrap.py` | `hub_proxy.c`(`boot_fetch`) | 부트스트랩을 진짜 클라우드에 다시 묻기 |
| `nas_hub.py` | `hub_main.c`(`app_main`) | 위를 공용 코어 하나로 조립(두 모드의 진입점) |
| `standalone/dns_hijack.py` | `hub_dns.c` | 등록된 기기의 rkiot 만 가로챔(`only_src`) |

## standalone 모드 (클라우드 없이 NAS 가 직접, ESP32 단독 모드와 동일)

`MODE=standalone` 이면 같은 `nas_hub.py` 가 클라우드에 붙지 않고 기기의 MQTT 상대가 된다.
린나이 앱으로는 보이지 않지만 인터넷이 끊겨도 SmartThings(LAN 드라이버)·API 는 그대로 된다.
클라우드가 하던 일은 허브가 대신한다(MITM 캡처로 확인한 것):

- CONNECT/SUBSCRIBE/PUBLISH/PING 에 CONNACK·SUBACK·PUBACK·PINGRESP 로 답한다(아이디·암호는 따지지 않음).
- 기기가 `<base>/get` 을 구독하면 곧바로, 그 뒤 30초마다 상태 요청(`fa`)을 보낸다.
- 기기의 시간 요청(`<base>/stg`, 약 1시간마다)에 `<base>/set` 의 `72` = 현재 한국 시각(`YYYYMMDDTHHMMSS`)으로 답한다.
- `/key/server` 는 그 기기의 저장된 답(프록시로 쓰던 때 받은 것), 없으면 허브가 만든 답을 준다.

프록시로 등록했던 기기는 그대로 단독으로 옮겨 붙는다. 단독에서 처음 등록한 기기는 나중에 프록시로 바꾸면
다시 등록이 필요할 수 있다(임시 아이디·암호를 받았기 때문). TLS 관문만 시험하려면 `GATE_TEST=1`(옛 `server_pi3.py`).

## 쓰기

1. 공유기에서 NAS 주소를 고정하고, 보일러·각방 조절기 IP도 고정한다.
2. 공유기 **DHCP 의 DNS 서버**를 `1차: NAS, 2차: 공유기`로.
3. `.env` 를 만든다:
   ```
   NAS_IP=192.168.0.10            # 이 NAS 의 LAN 주소
   RINNAI_IPS=192.168.0.20,192.168.0.21   # 보일러, 각방 조절기
   # FORWARD=none                 # (선택) 완전 독립: 다른 도메인도 안 넘김
   ```
4. 레포 루트에서:
   ```sh
   docker compose -f server/lynnai-nas/docker-compose.yml up -d --build
   ```
5. 보일러·각방 전원을 껐다 켜서 새 DNS 를 받게 한다.
6. SmartThings 앱에서 "기기 추가 → 주변 검색" → `edge-driver-lan`(LYNNAI Hub (LAN))이 NAS 를 찾는다.
   처음 붙는 드라이버는 자동 등록. 나중에 다른 드라이버를 붙이려면 `LAN_PAIR=1` 로 잠깐 열어 둔다.

`network_mode: host` 를 쓰는 이유: `:53`/`:8883` 가 집 LAN 에 직접 보여야 하고, DNS 가 **질의한 기기의
진짜 IP**를 봐야 `ONLY_SRC` 로 걸러낼 수 있기 때문이다(브리지 네트워크면 출발지가 다 도커 IP 로 보인다).

## 환경변수

| 변수 | 뜻 | 기본 |
|---|---|---|
| `NAS_IP` | 이 NAS 의 LAN 주소(DNS 가 답할 값) | (필수) |
| `RINNAI_IPS` / `ONLY_SRC` | 가로챌 기기 IP(쉼표). 비우면 모든 기기의 rkiot 질의 | 비움 |
| `HIJACK_NAMES` | 가로챌 도메인(쉼표, 부분일치) | `rkiot.net` |
| `FORWARD` | 목록 밖 질의를 넘길 상위 DNS. `none`=안 넘김 | 기본 게이트웨이 |
| `MODE` | `proxy`(클라우드 중계) / `standalone`(클라우드 없이) | `proxy` |
| `UPSTREAM_IP` | 진짜 클라우드 주소. 비우면 `www.rkiot.net` 을 DNS 로 조회(10분 재사용) | 비움 |
| `GATE_TEST` | 값이 있으면 TLS 핸드셰이크 성사/거부만 확인 | 꺼짐 |
| `UPSTREAM_CA` | proxy 모드에서 클라우드 인증서를 검증할 CA(린나이 사설 CA "ckstack"). 없거나 비우면 클라우드에 붙지 않음 | `cloud_ca.crt`(이 폴더) |
| `UPSTREAM_CN` | 클라우드 인증서에 있어야 하는 이름(CN) | `dev2.ckpush.com` |

> MAC 기반 제한은 도커에서 ARP 조회가 필요해 지금은 IP 기반이다. 기기 IP 를 공유기에서 고정하면 같은 효과.
