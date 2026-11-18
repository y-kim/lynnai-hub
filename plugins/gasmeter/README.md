# gasmeter plugin

가스 계량기의 리드스위치(건전지 없는 펄스 출력)를 라즈베리 파이 GPIO 로 세어
사용량과 **도시가스 요금**을 Home Assistant 로 보내는 선택 플러그인.
린나이 허브 코어(`appliance/`, `standalone/`)와 **독립**이며, 쓰지 않으면 설치하지 않아도 된다.

- **입력**: 리드스위치 → GPIO17 (기본). 보통 **1펄스 = 0.01 m³**(계량기 명판 확인).
- **출력**: 로컬 MQTT 로 사용량·요금 발행 + HA Energy 대시보드 디스커버리.
- **요금**: 한국 도시가스는 열량(MJ) 과금이라, 공급사 공개 자료에서 네 값을 받아 m³당 요금을 계산한다.
  `won/m³(부가세 전) = 온압보정계수 × 열량계수(MJ/m³) × 단가(원/MJ)` ( +VAT, +기본요금 ).

## 파일

| 파일 | 역할 |
|---|---|
| `gasmeter.py` | 펄스 카운터 본체(`run`), MQTT/HA 송신. `~/gas/mqtt.json` 에서 브로커 설정을 읽는다 |
| `gasprice.py` | 공급사 공개 페이지에서 단가/열량/보정계수를 받아 `~/gas/price.json` 생성(크론 1일 1회) |
| `gasprice_suppliers/` | 공급사 플러그인 한 파일 = 한 공급사. 현재 `koone` = 코원에너지서비스(서울·경기) |
| `gasmeter.service` | systemd 유닛(경로·User 수정 후 설치) |
| `i18n.py`, `locale/` | 메시지 번역(`locale/<lang>.json`) |

## 빠른 시작

```bash
# 1) 브로커 설정(한 번):  ~/gas/mqtt.json
#    {"host": "...", "port": 1883, "user": "...", "password": "...", "topic": "gasmeter"}
# 2) 요금(하루 1회 크론):
python3 gasprice.py --supplier koone --region 서울   # 처음 한 번: ~/gas/gasprice.json 에 기억
python3 gasprice.py                                   # 이후: 기억한 공급사/지역으로 재조회
# 3) 계량:
python3 gasmeter.py run --price ~/gas/price.json
```

> 다른 공급사는 `gasprice_suppliers/<이름>.py` 플러그인을 추가하면 된다(인터페이스는
> `gasprice_suppliers/__init__.py` 주석 참고).
