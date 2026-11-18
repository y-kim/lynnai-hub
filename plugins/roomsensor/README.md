# roomsensor plugin

온습도계: **SHT31-D**(I2C) 온도·습도를 읽어 로컬 MQTT 로 발행하고 Home Assistant 에 자동 등록하는
선택 플러그인. 린나이 허브 코어(`appliance/`, `standalone/`)와 **독립**이며, 가스미터 플러그인과
같은 방식(로컬 브로커 + HA 디스커버리)으로 붙는다.

## 배선 (Raspberry Pi ↔ SHT31-D)

| SHT31-D | Pi 핀 |
|---|---|
| VIN | 3V3 (핀 1) |
| GND | GND (핀 6) |
| SDA | GPIO2 / SDA (핀 3) |
| SCL | GPIO3 / SCL (핀 5) |

`sudo raspi-config` → Interface → I2C **활성화**. 확인: `i2cdetect -y 1` 에 `0x44` 가 보이면 OK.

## 설정 & 실행

```bash
# 브로커 설정(한 번):  ~/sensor/mqtt.json
#   {"host":"broker.lan","port":1883,"user":"...","password":"...",
#    "topic":"lynnai/sensor/living","discovery":"homeassistant"}

python3 roomsensor.py once                              # 배선 점검(값만 출력)
python3 roomsensor.py once --mqtt ~/sensor/mqtt.json    # 한 번 재서 발행
python3 roomsensor.py run  --mqtt ~/sensor/mqtt.json    # 서비스(기본 60초 간격)
```

- `--id <이름>` : 센서 구분자(HA `unique_id`/토픽 접미사). 방마다 다르게 주면 여러 개 운용 가능.
- `--interval N` : 재는 간격(초). `--addr 0x45` : 주소 핀을 올린 경우. `--bus N` : I2C 버스 번호.
- `discovery` 를 주면 HA 에 **온도(°C)·습도(%)** 센서 2개가 한 기기로 자동 등록된다(retained).

systemd 서비스는 `roomsensor.service`(경로·User·`--mqtt` 수정 후 설치).
