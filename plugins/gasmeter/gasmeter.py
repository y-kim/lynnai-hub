#!/usr/bin/env python3
"""Gas meter pulse counter (reed switch on the meter -> GPIO17; see README.md, "gas meter input").

    gasmeter.py run                 count pulses until stopped (run it as a service, see gasmeter.service)
    gasmeter.py status              meter reading, pulse count, last pulse, use in the last hour / day
    gasmeter.py set 1234.567        line the count up with the number on the meter's counter (m³)
    gasmeter.py live                print the switch state as it changes (wiring check, counts nothing)

One pulse = 10 dm³ (0.01 m³) by default; --step changes it (it is printed on the meter, e.g. "1 imp = 0.01 m³").
Files in ~/gas (--dir): state.json (count, calibration) and pulses.csv (one line per pulse: time, count).

With --mqtt <file>, `run` also publishes to an MQTT broker (needs paho-mqtt). The file is JSON:
    {"host": "broker.lan", "port": 1883, "user": "...", "password": "...", "topic": "gasmeter"}
<topic>/state (retained) is JSON: m3 (meter reading, null until `set`), volume (m3, or the counted volume
when not set), count, last (time of the last pulse), flow (m³/h from the pulse spacing), hour and day
(m³ used in the last hour / 24 h).
It is sent on start, on every pulse and once a minute. <topic>/status is "online", or "offline" when
`run` stops or loses the broker.

With --price <file> as well, the state also carries the cost (VAT included, rounded to won): day_won
(last 24 h) and month_won (this calendar month's use plus the monthly base charge), and month (m³ this
month). The file is JSON, read again on every send so another job can keep it current:
    {"won_per_m3": 957.12, "base": 1250, "vat": 0.1}       (won_per_m3 before VAT)
The state then also has price: won per m³ with VAT, for a dashboard that works out the cost itself.

Home Assistant: add "homeassistant": "homeassistant" (the discovery prefix) to the MQTT file and `run`
announces the meter by MQTT discovery: a gas sensor (m³, total_increasing, for the Energy dashboard's
gas consumption) and, with --price, a price sensor (KRW/m³) and this month's cost (KRW).

The switch closes to GND; GPIO17 has a 10k pull-up and 0.1 µF to GND on the board, so the pin reads 0 while
the magnet is at the switch. A pulse counts when the pin has been low for --low seconds after being high for
--high seconds, so contact bounce and a dial stopped at the magnet never count twice.
"""
import argparse
import csv
import json
import os
import sys
import time
from datetime import datetime, timedelta

from i18n import _

PIN_NAME = "GPIO17"
POLL = 0.02                     # s between reads; a pulse lasts ~1 s even at full flow
FLOW_STOP = 900                 # s without a pulse after which the flow reads 0 (Qmin 0.06 m³/h = 1 / 10 min)


def find_line(name):
    """(chip path, offset) of the GPIO line with this name, on any gpiochip."""
    import gpiod
    for dev in sorted(os.listdir("/dev")):
        if not dev.startswith("gpiochip"):
            continue
        path = "/dev/" + dev
        try:
            with gpiod.Chip(path) as chip:
                return path, chip.line_offset_from_id(name)
        except (OSError, ValueError):
            continue
    sys.exit(_("No GPIO line named {name} (is this a Raspberry Pi with libgpiod 2?)", name=name))


class Pin:
    """GPIO17 as an input with the internal pull-up on as well (harmless next to the 10k on the board)."""

    def __init__(self, name=PIN_NAME):
        import gpiod
        from gpiod.line import Bias, Direction
        path, self.off = find_line(name)
        self.req = gpiod.request_lines(path, consumer="gasmeter", config={
            self.off: gpiod.LineSettings(direction=Direction.INPUT, bias=Bias.PULL_UP)})

    def low(self):
        from gpiod.line import Value
        return self.req.get_value(self.off) == Value.INACTIVE


class State:
    def __init__(self, folder, step):
        self.dir, self.step = folder, step
        os.makedirs(folder, exist_ok=True)
        self.path = os.path.join(folder, "state.json")
        self.log = os.path.join(folder, "pulses.csv")
        try:
            with open(self.path, encoding="utf-8") as f:
                self.d = json.load(f)
        except (OSError, ValueError):
            self.d = {"count": 0, "base_m3": None, "base_count": 0, "last": None}
        self.gap = None         # seconds between the last two pulses seen by this process

    def m3(self):
        if self.d["base_m3"] is None:
            return None
        return round(self.d["base_m3"] + (self.d["count"] - self.d["base_count"]) * self.step, 3)

    def save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.d, f, indent=1)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, self.path)
        fd = os.open(self.dir, os.O_RDONLY)     # make the rename itself survive a power cut
        try:
            os.fsync(fd)
        finally:
            os.close(fd)

    def reload(self):
        """Pick up a `set` made while `run` is going."""
        try:
            with open(self.path, encoding="utf-8") as f:
                d = json.load(f)
            self.d["base_m3"], self.d["base_count"] = d["base_m3"], d["base_count"]
        except (OSError, ValueError, KeyError):
            pass

    def flow(self):
        """Flow in m³/h from the pulse spacing. While no pulse comes it falls as 1 / the time since the
        last one, so it reaches 0 smoothly; after FLOW_STOP seconds without a pulse it is 0."""
        if not self.d["last"]:
            return 0.0
        since = (datetime.now().astimezone() - datetime.fromisoformat(self.d["last"])).total_seconds()
        if since > FLOW_STOP:
            return 0.0
        gap = max(self.gap or since, since, 1.0)
        return round(self.step * 3600 / gap, 3)

    def pulse(self):
        if self.d["last"]:
            prev = datetime.fromisoformat(self.d["last"])
            self.gap = (datetime.now().astimezone() - prev).total_seconds()
        now = datetime.now().astimezone().isoformat(timespec="seconds")
        self.reload()
        self.d["count"] += 1
        self.d["last"] = now
        self.save()
        with open(self.log, "a", encoding="utf-8") as f:
            f.write(f"{now},{self.d['count']}\n")
            f.flush()
            os.fsync(f.fileno())
        return now

    def pulses_since(self, t):
        """Pulses logged at or after datetime t. Lines a power cut left half-written (NUL bytes) are skipped."""
        n = 0
        try:
            with open(self.log, encoding="utf-8", errors="replace") as f:
                for row in csv.reader(line.replace("\0", "") for line in f):
                    try:
                        if row and datetime.fromisoformat(row[0]) >= t:
                            n += 1
                    except ValueError:
                        continue
        except OSError:
            pass
        return n


class Publisher:
    """Sends the meter state to an MQTT broker; reconnects by itself (paho's loop thread)."""

    def __init__(self, path, st, price=None):
        import paho.mqtt.client as mqtt
        with open(os.path.expanduser(path), encoding="utf-8") as f:
            cfg = json.load(f)
        self.st, self.topic, self.price = st, cfg.get("topic", "gasmeter"), price
        self.ha = cfg.get("homeassistant")         # discovery prefix, or None
        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="gasmeter-" + os.uname().nodename)
        if cfg.get("user"):
            self.c.username_pw_set(cfg["user"], cfg.get("password"))
        self.c.will_set(self.topic + "/status", "offline", qos=1, retain=True)
        self.c.on_connect = lambda c, u, f, rc, p=None: self.connected(rc)
        self.c.connect_async(cfg["host"], int(cfg.get("port", 1883)), keepalive=60)
        self.c.loop_start()

    def connected(self, rc):
        if rc == 0:
            if self.ha:
                self.announce()
            self.c.publish(self.topic + "/status", "online", qos=1, retain=True)
            self.send()
        else:
            print(_("MQTT connection refused: {rc}", rc=rc), flush=True)

    def send(self):
        st, now = self.st, datetime.now().astimezone()
        m3 = st.m3()
        msg = {"m3": m3, "volume": m3 if m3 is not None else round(st.d["count"] * st.step, 3),
               "count": st.d["count"], "last": st.d["last"], "flow": st.flow(),
               "hour": round(st.pulses_since(now - timedelta(hours=1)) * st.step, 3),
               "day": round(st.pulses_since(now - timedelta(days=1)) * st.step, 3)}
        p = self.load_price()
        if p:
            month_start = now.replace(day=1, hour=0, minute=0, second=0, microsecond=0)
            month = st.pulses_since(month_start) * st.step
            vat = 1 + float(p.get("vat", 0.1))
            unit = float(p["won_per_m3"])
            msg.update(month=round(month, 3), day_won=round(msg["day"] * unit * vat),
                       month_won=round((month * unit + float(p.get("base", 0))) * vat),
                       price=round(unit * vat, 2))
        self.c.publish(self.topic + "/state", json.dumps(msg), qos=1, retain=True)

    def announce(self):
        """Home Assistant MQTT discovery: one device with the meter, its price and this month's cost."""
        node = self.topic.replace("/", "_")
        device = {"identifiers": [node], "name": _("Gas meter"), "model": "reed switch pulse counter"}
        common = {"state_topic": self.topic + "/state", "availability_topic": self.topic + "/status",
                  "device": device}
        sensors = {
            "flow": {"name": _("Gas flow"), "device_class": "volume_flow_rate", "state_class": "measurement",
                     "unit_of_measurement": "m³/h", "value_template": "{{ value_json.flow }}",
                     "suggested_display_precision": 2},
            "volume": {"name": _("Meter reading"), "device_class": "gas", "state_class": "total_increasing",
                       "unit_of_measurement": "m³", "value_template": "{{ value_json.volume }}",
                       "suggested_display_precision": 2},
        }
        if self.price:
            sensors["price"] = {"name": _("Gas price"), "unit_of_measurement": "KRW/m³",
                                "value_template": "{{ value_json.price | default(none) }}"}
            sensors["month_won"] = {"name": _("Cost this month"), "device_class": "monetary",
                                    "unit_of_measurement": "KRW", "state_class": "total",
                                    "value_template": "{{ value_json.month_won | default(none) }}"}
        for key, conf in sensors.items():
            conf = {**common, **conf, "unique_id": f"{node}_{key}",
                    # entity id sensor.<node>_<key>; object_id for older Home Assistant versions
                    "default_entity_id": f"sensor.{node}_{key}", "object_id": f"{node}_{key}"}
            self.c.publish(f"{self.ha}/sensor/{node}/{key}/config", json.dumps(conf, ensure_ascii=False),
                           qos=1, retain=True)

    def load_price(self):
        if not self.price:
            return None
        try:
            with open(os.path.expanduser(self.price), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def close(self):
        self.c.publish(self.topic + "/status", "offline", qos=1, retain=True).wait_for_publish(3)
        self.c.loop_stop()
        self.c.disconnect()


def cmd_run(a, st):
    pin = Pin()
    pub = Publisher(a.mqtt, st, a.price) if a.mqtt else None
    try:
        count_pulses(a, st, pin, pub)
    finally:
        if pub:
            pub.close()


def count_pulses(a, st, pin, pub):
    need_high, need_low = a.high, a.low
    armed = False               # True once the pin has been high long enough
    since = time.monotonic()
    was_low = pin.low()
    next_send = since + 60
    print(_("Counting pulses on {pin}; count so far {count}, meter {m3}", pin=PIN_NAME,
            count=st.d["count"], m3=fmt_m3(st.m3())), flush=True)
    while True:
        time.sleep(POLL)
        now = time.monotonic()
        if pub and now >= next_send:          # keeps hour / day use current while no gas flows
            st.reload()
            pub.send()
            next_send = now + 60
        lo = pin.low()
        if lo != was_low:
            was_low, since = lo, now
            continue
        held = now - since
        if not lo and not armed and held >= need_high:
            armed = True
        elif lo and armed and held >= need_low:
            armed = False
            ts = st.pulse()
            if pub:
                pub.send()
            if a.verbose:
                print(_("{ts} pulse {count}, meter {m3}", ts=ts, count=st.d["count"], m3=fmt_m3(st.m3())), flush=True)


def cmd_live(a, st):
    pin = Pin()
    last = None
    print(_("Ctrl+C to stop. Turn on the hot water and watch for 'closed' each time the last dial digit passes 0."))
    while True:
        lo = pin.low()
        if lo != last:
            print(time.strftime("%T"), _("closed (magnet at the switch)") if lo else _("open"), flush=True)
            last = lo
        time.sleep(POLL)


def fmt_m3(v):
    return _("not set (gasmeter.py set <m³>)") if v is None else f"{v:.3f} m³"


def cmd_status(a, st):
    now = datetime.now().astimezone()
    h, d = st.pulses_since(now - timedelta(hours=1)), st.pulses_since(now - timedelta(days=1))
    print(_("Meter reading: {m3}", m3=fmt_m3(st.m3())))
    print(_("Pulses counted: {count} (1 pulse = {step} m³)", count=st.d["count"], step=st.step))
    print(_("Last pulse: {ts}", ts=st.d["last"] or "-"))
    print(_("Last hour: {m3:.2f} m³, last 24 h: {d:.2f} m³", m3=h * st.step, d=d * st.step))


def cmd_set(a, st):
    st.d["base_m3"], st.d["base_count"] = a.m3, st.d["count"]
    st.save()
    print(_("Meter reading set to {m3}", m3=fmt_m3(st.m3())))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dir", default=os.path.expanduser("~/gas"), help=_("Folder for state.json and pulses.csv"))
    p.add_argument("--step", type=float, default=0.01, help=_("m³ per pulse (printed on the meter)"))
    p.add_argument("--mqtt", metavar="FILE", help=_("JSON file with the MQTT broker to publish to (run only)"))
    p.add_argument("--price", metavar="FILE", help=_("JSON file with the gas price, to publish the cost too"))
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--high", type=float, default=0.3, help=_("Seconds the switch must stay open before the next pulse"))
    r.add_argument("--low", type=float, default=0.1, help=_("Seconds the switch must stay closed to count"))
    r.add_argument("-v", "--verbose", action="store_true", help=_("Print every pulse"))
    sub.add_parser("status")
    sub.add_parser("live")
    s = sub.add_parser("set")
    s.add_argument("m3", type=float, help=_("Number on the meter's counter now (m³)"))
    a = p.parse_args()
    st = State(a.dir, a.step)
    try:
        {"run": cmd_run, "status": cmd_status, "set": cmd_set, "live": cmd_live}[a.cmd](a, st)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
