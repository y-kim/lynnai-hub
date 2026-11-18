#!/usr/bin/env python3
"""온습도계 플러그인 — SHT31-D (I2C 0x44) 온도·습도를 로컬 MQTT + Home Assistant 로.

린나이 허브 코어와 독립이며, 가스미터 플러그인과 같은 방식(로컬 브로커 + HA 디스커버리)으로
붙는다. SHT31-D 를 라즈베리 파이 I2C(SDA=GPIO2, SCL=GPIO3, 주소 0x44)에 연결하고
`sudo raspi-config` 에서 I2C 를 켠다.

    roomsensor.py run  --mqtt ~/sensor/mqtt.json     # 간격마다 재서 발행(서비스)
    roomsensor.py once --mqtt ~/sensor/mqtt.json     # 한 번만 재서 발행하고 출력
    roomsensor.py once                               # 브로커 없이 센서값만 출력(배선 점검)

mqtt.json 예:
    {"host":"broker.lan","port":1883,"user":"...","password":"...",
     "topic":"lynnai/sensor/living","discovery":"homeassistant"}
('discovery' 를 주면 HA 에 온도/습도 센서를 자동 등록한다. 없으면 topic 에 값만 발행.)
"""
import argparse
import json
import os
import sys
import time

ADDR_DEFAULT = 0x44


def crc8(data):
    """SHT3x 체크섬(다항식 0x31, 초기값 0xFF)."""
    c = 0xFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x31) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c


def read_sht31(bus, addr):
    """단발 측정(고정밀, 클럭스트레치 없음) → (섭씨, 상대습도%). CRC 불일치면 IOError."""
    from smbus2 import i2c_msg
    bus.i2c_rdwr(i2c_msg.write(addr, [0x24, 0x00]))
    time.sleep(0.02)
    r = i2c_msg.read(addr, 6)
    bus.i2c_rdwr(r)
    d = list(r)
    if crc8(d[0:2]) != d[2] or crc8(d[3:5]) != d[5]:
        raise IOError("SHT31 CRC mismatch")
    t = -45 + 175 * ((d[0] << 8) | d[1]) / 65535
    h = 100 * ((d[3] << 8) | d[4]) / 65535
    return round(t, 2), round(h, 1)


class Sink:
    """로컬 MQTT 발행 + (설정 시) Home Assistant 디스커버리. 브로커 설정 없으면 무동작."""
    def __init__(self, cfg, dev_id):
        self.cfg = cfg or {}
        self.dev_id = dev_id
        self.topic = self.cfg.get("topic", "lynnai/sensor/" + dev_id)
        self.c = None
        if self.cfg.get("host"):
            import paho.mqtt.client as mqtt
            self.c = mqtt.Client()
            if self.cfg.get("user"):
                self.c.username_pw_set(self.cfg["user"], self.cfg.get("password"))
            self.c.connect(self.cfg["host"], int(self.cfg.get("port", 1883)), 30)
            self.c.loop_start()
            self._announce()

    def _announce(self):
        """HA MQTT 디스커버리: 온도/습도 센서 2개를 한 기기로 등록(retained)."""
        disc = self.cfg.get("discovery")
        if not disc:
            return
        dev = {"identifiers": ["lynnai_" + self.dev_id], "name": "LYNNAI " + self.dev_id,
               "manufacturer": "Lynnai", "model": "SHT31 sensor"}
        for key, unit, dclass, field in (("temp", "°C", "temperature", "temp"),
                                         ("hum", "%", "humidity", "hum")):
            ctopic = "%s/sensor/lynnai_%s_%s/config" % (disc, self.dev_id, key)
            payload = {
                "name": "%s %s" % (self.dev_id, dclass),
                "unique_id": "lynnai_%s_%s" % (self.dev_id, key),
                "state_topic": self.topic,
                "value_template": "{{ value_json.%s }}" % field,
                "unit_of_measurement": unit, "device_class": dclass,
                "state_class": "measurement", "device": dev,
            }
            self.c.publish(ctopic, json.dumps(payload), retain=True)

    def send(self, temp, hum):
        if self.c:
            self.c.publish(self.topic, json.dumps({"temp": temp, "hum": hum}), retain=True)


def loop(args, sink):
    from smbus2 import SMBus
    once = args.cmd == "once"
    with SMBus(args.bus) as bus:
        while True:
            try:
                t, h = read_sht31(bus, args.addr)
                sink.send(t, h)
                if once or args.verbose:
                    print(time.strftime("%F %T"), "temp=%.2f°C hum=%.1f%%" % (t, h), flush=True)
            except Exception as e:                      # 한 번 실패해도 다음 주기에 재시도
                print(time.strftime("%F %T"), "fail:", e, flush=True)
                if once:
                    sys.exit(1)
            if once:
                return
            time.sleep(args.interval - time.time() % args.interval)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cmd", choices=["run", "once"], help="run=반복 발행, once=한 번만")
    p.add_argument("--mqtt", metavar="FILE", help="브로커 설정 JSON(host/port/user/password/topic/discovery)")
    p.add_argument("--id", default="sht31", help="센서 id (HA unique_id/토픽 접미사)")
    p.add_argument("--addr", type=lambda x: int(x, 0), default=ADDR_DEFAULT, help="I2C 주소 (기본 0x44)")
    p.add_argument("--bus", type=int, default=1, help="I2C 버스 번호 (기본 1)")
    p.add_argument("--interval", type=int, default=60, help="재는 간격 초 (run)")
    p.add_argument("-v", "--verbose", action="store_true", help="매 측정 출력")
    args = p.parse_args()
    cfg = {}
    if args.mqtt:
        with open(os.path.expanduser(args.mqtt), encoding="utf-8") as f:
            cfg = json.load(f)
    loop(args, Sink(cfg, args.id))


if __name__ == "__main__":
    main()
