"""Wire format of the LYNNAI hub's LAN channel (no Home Assistant imports here).

The hub relays the Rinnai devices' cloud link and forwards what it sees:

    hub -> us   0x01 state    [kind 6|9][last 3 MAC bytes][EPC 70 blob]
                0x02 links    ([kind][mac3]) * n       devices connected right now
                0x03 room     int16 x2                 temperature, humidity (x100; -32768 = no humidity)
                0x04 gas      uint32 x5                pulses, meter litres (0xFFFFFFFF = not set), last hour L, last 24 h L, flow L/h
                0x05 plugins  [enabled][faulty][paused]  bit 1 = room sensor, bit 2 = gas meter
                0x07 pairs    ([f09 mac3][f06 mac3]) * n   which thermostat belongs to which boiler
                0x08 forget   [kind][mac3]             device deleted on the hub
                0x0a alive    keep-alive
    us -> hub   0x00 request  text: "f06@<mac> b1=1", "f09@<mac> a0=7,d4=17", "refresh"

kind 6 (f06) is a boiler controller, kind 9 (f09) a room thermostat controller with up to seven rooms.
This mirrors edge-driver/src/core.lua, the SmartThings driver that speaks the same protocol.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import struct

CMD_REQUEST = 0x00
CMD_STATE = 0x01
CMD_LINKS = 0x02
CMD_ROOM = 0x03
CMD_GAS = 0x04
CMD_PLUGINS = 0x05
CMD_EXPOSE = 0x06
CMD_PAIRS = 0x07
CMD_FORGET = 0x08
CMD_TARIFF = 0x09
CMD_ALIVE = 0x0A
CMD_MODE = 0x0B
CMD_BOILCFG = 0x0C   # boiler settings: [hot water preheat auto-off, minutes, 0 = never]

KIND_BOILER = 6
KIND_THERMOSTAT = 9

PLUGIN_ROOM = 1
PLUGIN_GAS = 2

NO_HUMIDITY = -32768
GAS_UNSET = 0xFFFFFFFF

HEAT_MODE_ONDOL = "ondol"   # floor (water) temperature control, 40-85 C
HEAT_MODE_ROOM = "room"     # room temperature control, 5-40 C

# Temperatures the boiler accepts (the device ignores values in between).
ONDOL_TEMPS = [40, 45, *range(46, 61), 65, 70, 75, 80, 85]   # 35 is not taken (bounces back to 40)
ROOM_TEMPS = list(range(5, 41))
HOT_WATER_TEMPS = [*range(35, 48), 50, 55, 60]   # the only values the boiler takes (others bounce back after ~20 s)
ZONE_MIN_TEMP = 5
ZONE_MAX_TEMP = 40

# Boiler switches: position of the flag in the hex string, and the command code.
BOILER_SWITCHES = {
    "power": (0, "80"),
    "heat": (2, "b1"),
    "hotwater": (3, "d7"),
    "preheat": (5, "b6"),
}


@dataclass(frozen=True)
class ZoneSlot:
    """One room slot inside a thermostat controller's state blob."""

    zone_id: str
    number: int     # 1-6 = room 1-6, 7 = living room (also the a0 selector in commands)
    stat: int       # byte offset of the status byte; current temp is +3, target +4


# Slot offsets are 7 bytes apart; ids z1..z7 are the ones the SmartThings driver uses.
ZONE_SLOTS = {
    "z1": ZoneSlot("z1", 1, 0),
    "z2": ZoneSlot("z2", 2, 7),
    "z4": ZoneSlot("z4", 3, 14),
    "z5": ZoneSlot("z5", 4, 21),
    "z6": ZoneSlot("z6", 5, 28),
    "z7": ZoneSlot("z7", 6, 35),
    "z3": ZoneSlot("z3", 7, 42),
}


@dataclass
class BoilerState:
    power: bool = False
    heat: bool = False
    hotwater: bool = False
    preheat: bool = False
    heat_mode: str = HEAT_MODE_ONDOL
    hot_water_setpoint: int | None = None
    ondol_setpoint: int | None = None
    room_setpoint: int | None = None
    heat_burning: bool = False
    hot_water_burning: bool = False

    @property
    def heat_setpoint(self) -> int | None:
        return self.room_setpoint if self.heat_mode == HEAT_MODE_ROOM else self.ondol_setpoint


@dataclass
class ZoneState:
    on: bool
    heating: bool
    away: bool
    reserved: bool
    reserve_hours: int
    time_mode: bool             # heats a few minutes every hour instead of to a temperature
    current: int
    target: int
    time_minutes: int = 10      # time mode: minutes of heating every hour (kept while the mode is off too)


@dataclass
class GasState:
    pulses: int
    meter_m3: float | None      # None until the meter reading was set on the hub
    hour_m3: float
    day_m3: float
    flow_m3h: float

    @property
    def total_m3(self) -> float:
        """Meter reading if calibrated, else pulses x 0.01 m3 (the usual pulse size)."""
        return self.meter_m3 if self.meter_m3 is not None else round(self.pulses * 0.01, 2)


@dataclass
class RoomSensorState:
    temperature: float
    humidity: float | None


@dataclass
class PluginState:
    enabled: int = 0
    faulty: int = 0
    paused: int = 0

    def present(self, bit: int) -> bool:
        return bool(self.enabled & bit)

    def ok(self, bit: int) -> bool:
        return self.present(bit) and not (self.faulty & bit)


def mac_hex(b: bytes) -> str:
    return b.hex()


def decode_boiler(blob: bytes) -> BoilerState | None:
    """Decode a boiler (f06) state blob. Fields are nibbles of the hex string."""
    raw = blob.hex()
    if len(raw) < 32:
        return None

    def flag(i: int) -> bool:
        return raw[i] == "1"

    def num(i: int, j: int) -> int:
        return int(raw[i:j], 16)

    s = BoilerState()
    for name, (pos, _code) in BOILER_SWITCHES.items():
        setattr(s, name, flag(pos))
    s.heat_mode = HEAT_MODE_ROOM if flag(7) else HEAT_MODE_ONDOL
    s.heat_burning = flag(8)
    s.hot_water_burning = flag(9)
    s.hot_water_setpoint = num(10, 12)
    s.ondol_setpoint = num(13, 15)
    s.room_setpoint = num(15, 17)
    return s


def decode_zones(blob: bytes) -> dict[str, ZoneState]:
    """Decode a thermostat controller (f09) blob into the rooms it reports."""
    out: dict[str, ZoneState] = {}
    for zone_id, slot in ZONE_SLOTS.items():
        if len(blob) < slot.stat + 5:
            continue
        st, cur, target = blob[slot.stat], blob[slot.stat + 3], blob[slot.stat + 4]
        if st == 0 and cur == 0:        # empty slot: no room there
            continue
        out[zone_id] = ZoneState(
            on=bool(st & 0x02),
            heating=bool(st & 0x40),
            away=bool(st & 0x10),
            reserved=bool(st & 0x08),
            reserve_hours=blob[slot.stat + 1],
            time_mode=bool(st & 0x04),
            current=cur,
            target=target,
            time_minutes=blob[slot.stat + 5] if len(blob) > slot.stat + 5 else 10,
        )
    return out


def decode_room(payload: bytes) -> RoomSensorState | None:
    if len(payload) < 4:
        return None
    t100, h100 = struct.unpack_from("<hh", payload)
    return RoomSensorState(round(t100 / 100, 1), None if h100 == NO_HUMIDITY else round(h100 / 100))


def decode_alive(payload: bytes) -> float | None:
    """[0][the hub's own chip temperature, s16 in 1/100 °C] - the temperature only when the hub is set to show it."""
    if len(payload) < 3:
        return None
    return round(struct.unpack_from("<h", payload, 1)[0] / 100, 1)


def decode_gas(payload: bytes) -> GasState | None:
    if len(payload) < 20:
        return None
    pulses, reading_l, hour_l, day_l, flow_lph = struct.unpack_from("<5I", payload)
    return GasState(
        pulses=pulses,
        meter_m3=None if reading_l == GAS_UNSET else round(reading_l / 1000, 2),
        hour_m3=round(hour_l / 1000, 2),
        day_m3=round(day_l / 1000, 2),
        flow_m3h=round(flow_lph / 1000, 2),
    )


def decode_plugins(payload: bytes) -> PluginState:
    b = payload + b"\x00\x00\x00"       # older firmware sends one or two bytes
    return PluginState(b[0], b[1], b[2])


def decode_links(payload: bytes) -> set[tuple[int, str]]:
    out = set()
    for i in range(0, len(payload) - 3, 4):
        kind = payload[i]
        if kind in (KIND_BOILER, KIND_THERMOSTAT):
            out.add((kind, payload[i + 1 : i + 4].hex()))
    return out


def decode_pairs(payload: bytes) -> dict[str, str]:
    """thermostat mac -> boiler mac ("000000" = marked stand-alone on the hub)."""
    return {payload[i : i + 3].hex(): payload[i + 3 : i + 6].hex() for i in range(0, len(payload) - 5, 6)}


def snap(value: float, allowed: list[int], current: int | None = None) -> int:
    """Pick the allowed temperature for a request; step in the direction of the change."""
    v = round(value)
    if current is not None and v > current:
        for a in allowed:
            if a >= v:
                return a
    if current is not None and v < current:
        for a in reversed(allowed):
            if a <= v:
                return a
    return min(allowed, key=lambda a: abs(a - v))


# ---- hub operating mode ----

HUB_MODE_PROXY = "proxy"            # relays the devices to the Rinnai cloud (the Rinnai app keeps working)
HUB_MODE_STANDALONE = "standalone"  # the hub answers the devices itself, no cloud


@dataclass(frozen=True)
class HubMode:
    mode: str               # what the hub is set to
    fallback: bool          # set to proxy, but the cloud is unreachable so it is serving the devices itself for now
    auto_fallback: bool     # whether it may do that on its own
    since: int | None = None    # when the fallback began (unix seconds); None when not in fallback or unknown
    reason: str | None = None   # why the server counts as unreachable (FALLBACK_REASONS); None when not in fallback or unknown
    code: int = 0               # the server's refusal code when the reason is "refused"

    @property
    def status(self) -> str:
        """What the hub is doing right now: 'proxy', 'standalone', or 'fallback' (set to proxy, serving locally)."""
        if self.mode == HUB_MODE_STANDALONE:
            return HUB_MODE_STANDALONE
        return "fallback" if self.fallback else HUB_MODE_PROXY


# Why the hub fell back, by the number it sends (hub_proxy.c FB_WHY_*): the home Wi-Fi is down, the server cannot be
# connected to, the secure connection fails, the server refuses or does not answer the device's login, or it closes
# the connection before accepting it, or its address cannot be looked up.
FALLBACK_REASONS = {1: "wifi", 2: "connect", 3: "tls", 4: "refused", 5: "noreply", 6: "closed", 7: "dns"}


def decode_mode(payload: bytes) -> HubMode | None:
    """[mode 0 proxy | 1 standalone][fallback active][auto fallback setting][fallback start, u32 unix seconds or 0]
    [reason][refusal code] - the last two only from newer hubs."""
    if len(payload) < 3:
        return None
    fallback = payload[1] != 0
    since = struct.unpack_from("<I", payload, 3)[0] if len(payload) >= 7 else 0
    reason = FALLBACK_REASONS.get(payload[7]) if fallback and len(payload) >= 9 else None
    return HubMode(HUB_MODE_STANDALONE if payload[0] == 1 else HUB_MODE_PROXY, fallback, payload[2] != 0,
                   since if since and fallback else None, reason, payload[8] if reason == "refused" else 0)


# Hot water preheat never turns itself off and fires the burner every ten minutes or so while it is on; the hub
# turns it off after this many minutes (0 = never). The same choices as on the hub's admin page.
PREHEAT_OFF_CHOICES = [*range(1, 61), 0]   # minutes, 0 = never
# time mode: minutes of heating every hour (f09 da, two hex digits) — the Rinnai app offers exactly these
TIME_MINUTES_CHOICES = [5, 10, 20, 30, 40, 50, 60]


def time_minutes(n: int | None) -> int:
    """A device or remembered value -> the nearest choice (10 when unknown)."""
    if not n or n < 1:
        return 10
    return min(TIME_MINUTES_CHOICES, key=lambda v: (abs(v - n), v))


def preheat_off_option(minutes: int) -> str:
    return "unlimited" if minutes == 0 else str(minutes)


def preheat_off_minutes(option: str) -> int | None:
    if option == "unlimited":
        return 0
    return int(option) if option.isdigit() and int(option) in PREHEAT_OFF_CHOICES else None


BOILER_MODE_UNSET, BOILER_MODE_WATER, BOILER_MODE_ROOM = 0, 1, 2   # set on the hub (setup / admin page)


def decode_boilcfg(payload: bytes) -> int | None:
    """[preheat auto-off minutes, 0 = never][boiler mode, newer firmware]."""
    return payload[0] if payload and payload[0] in PREHEAT_OFF_CHOICES else None


def decode_preheat_left(payload: bytes) -> int | None:
    """Third byte: 0 preheat off, 1-60 minutes until the hub turns it off, 255 on without a limit; None = older firmware."""
    return payload[2] if len(payload) >= 3 else None


def decode_boiler_mode(payload: bytes) -> int:
    """Second byte: 1 heating water (room thermostats do the rooms), 2 floor/room; 0 or missing = not set."""
    return payload[1] if len(payload) >= 2 and payload[1] in (BOILER_MODE_WATER, BOILER_MODE_ROOM) else BOILER_MODE_UNSET


# ---- requests (text sent with CMD_REQUEST) ----

def hub_mode_cmd(mode: str) -> str:
    return f"hub mode={mode}"


def hub_fallback_cmd(on: bool) -> str:
    return f"hub fallback={onoff(on)}"


def hub_preheat_off_cmd(minutes: int) -> str:
    return f"hub preoff={minutes}"


def boiler_cmd(mac: str, code: str, value: str) -> str:
    return f"f06@{mac} {code}={value}"


def zone_cmd(mac: str, zone_id: str, fields: str) -> str:
    return f"f09@{mac} a0={ZONE_SLOTS[zone_id].number},{fields}"


def onoff(on: bool) -> str:
    return "1" if on else "0"


def encode_line(cmd: int, payload: bytes) -> bytes:
    return f"{cmd:02x} {payload.hex()}\n".encode()


def parse_line(line: str) -> tuple[int, bytes] | None:
    """'<cmd hex2> <payload hex>' -> (cmd, payload); None for anything else (e.g. 'ping')."""
    cmd, sep, body = line.strip().partition(" ")
    if len(cmd) != 2 or (body and not sep):
        return None
    try:
        return int(cmd, 16), bytes.fromhex(body)
    except ValueError:
        return None
