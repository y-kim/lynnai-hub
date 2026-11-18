# standalone — rkiot 클라우드 흉내 박스 (Pi3 검증 → Pico2W 포팅)

보일러 WiFi 모듈이 `rkiot.net:8883` MQTT/TLS 상시연결로 받는 명령을,
클라우드 대신 **우리 박스**가 내려주게 가로채는 로컬 서버.

## 파일 구조 (포팅성 기준 분리)

| 파일 | 역할 | Pico 포팅 |
|---|---|---|
| `mqtt_proto.py` | MQTT 3.1.1 패킷 인/디코드 | ✅ 그대로 (struct만) |
| `broker.py` | 최소 브로커 상태머신 (transport 무관) | ✅ 그대로 |
| `dns_hijack.py` | rkiot.net → 우리IP UDP DNS 응답 | ✅ socket→usocket만 |
| `server_pi3.py` | **Pi3 전용** TLS+소켓+select 글루 | ⟳ MicroPython ssl로 교체 |
| `gencert.sh` | self-signed 인증서(rkiot.net 사칭) | — |
| `ap/` | hostapd/dnsmasq (Pi3 AP 플럼빙) | — (Pico는 AP_IF 펌웨어) |

## ★ 1단계: TLS 핀닝 관문 테스트 (make-or-break)

보일러가 우리 self-signed 인증서를 **수락하면** 이 경로(AP+DNS+프록시 MITM)가 열리고,
**거부하면**(cert pinning) 이 길은 막혀서 **버스 직결**(boilerctl/capture Pico)만 남는다.

```sh
./gencert.sh                               # certs/ 생성
# (사용자 승인 후) AP 올리기:
sudo ./ap/setup-ap.sh                       # uap0 AP, wlan0 SSH 유지
# 서버 가동 — 핸드셰이크 성사/거부만 관찰:
sudo python3 server_pi3.py --config config.json --gate-test
# 보일러를 'rkiot-test' AP에 페어링 → 로그 확인:
#   ✅ HANDSHAKE OK  → 핀닝 없음, 경로 열림
#   ❌ HANDSHAKE FAIL → 핀닝 의심, 버스직결로
# 롤백(SSH 복구): sudo ./ap/teardown-ap.sh
```

## 2단계: 브로커 가동 (관문 통과 시)

```sh
sudo python3 server_pi3.py --config config.json
```
- 보일러 CONNECT/SUBSCRIBE 전부 accept, PUBLISH(상태 J07) 로그/덤프.
- 명령 하향은 `broker.publish_to(session, topic, j07bytes)` — `server_pi3.on_publish`/콜백에서 호출.
- J07 조립은 `rinnai-client/rinnai.py` 로직 이식 예정.

## 자체검증

순수 로직은 `python3`에서 바로 테스트 가능 (소켓/보일러 불필요):
CONNECT→CONNACK, SUBSCRIBE→SUBACK, 하향 PUBLISH, DNS A레코드, 스트림 분할 재조립 전부 확인됨.

## 주의
- `ap/hostapd.conf` 의 `channel=` 은 **집 공유기 채널과 동일**해야 함 (brcmfmac AP+STA 제약).
- cert pinning이면 여기까지가 한계 — 물리 버스 캡처(20.6kHz)가 유일한 독립 경로.
- 클라우드 자격증명/계정 정보는 이 공개성 폴더에 넣지 말 것.
