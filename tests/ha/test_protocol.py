"""Decoding and request building, against captured blobs."""

import struct

from custom_components.lynnai_hub import protocol as p

from .conftest import F06_OFF, F06_ON, f09


def test_boiler_on():
    s = p.decode_boiler(bytes.fromhex(F06_ON))
    assert (s.power, s.heat, s.hotwater, s.preheat) == (True, True, True, False)
    assert s.heat_mode == p.HEAT_MODE_ONDOL
    assert (s.hot_water_setpoint, s.ondol_setpoint, s.heat_setpoint) == (60, 65, 65)
    assert not s.heat_burning and not s.hot_water_burning


def test_boiler_off_and_short_blob():
    s = p.decode_boiler(bytes.fromhex(F06_OFF))
    assert (s.power, s.heat, s.hotwater) == (False, False, True)
    assert p.decode_boiler(b"\x10\x11") is None


def test_boiler_room_mode_and_burners():
    raw = list(F06_ON)
    raw[7], raw[8], raw[9] = "1", "1", "1"
    raw[15:17] = "16"                           # room setpoint 0x16 = 22
    s = p.decode_boiler(bytes.fromhex("".join(raw)))
    assert s.heat_mode == p.HEAT_MODE_ROOM and s.heat_setpoint == 22
    assert s.heat_burning and s.hot_water_burning


def test_zones():
    blob = bytearray.fromhex(f09(z1=0x83 | 0x40, z2=0x81 | 0x10, living=0x8B))
    blob[43] = 5                                # reservation length of the living room
    zones = p.decode_zones(bytes(blob))
    assert set(zones) == {"z1", "z2", "z3"}     # only the slots that report
    assert zones["z1"].on and zones["z1"].heating and (zones["z1"].current, zones["z1"].target) == (20, 23)
    assert not zones["z2"].on and zones["z2"].away
    assert zones["z3"].reserved and zones["z3"].reserve_hours == 5


def test_room_and_gas():
    r = p.decode_room(struct.pack("<hh", 2534, 3410))
    assert (r.temperature, r.humidity) == (25.3, 34)
    assert p.decode_room(struct.pack("<hh", 2475, p.NO_HUMIDITY)).humidity is None
    g = p.decode_gas(struct.pack("<5I", 1504, 0xFFFFFFFF, 210, 5190, 857))
    assert g.meter_m3 is None and g.total_m3 == 15.04
    assert (g.hour_m3, g.day_m3, g.flow_m3h) == (0.21, 5.19, 0.86)
    assert p.decode_gas(struct.pack("<5I", 1504, 1523400, 0, 0, 0)).total_m3 == 1523.4


def test_links_pairs_plugins():
    links = p.decode_links(bytes([6]) + bytes.fromhex("a1b2c3") + bytes([9]) + bytes.fromhex("d4e5f6"))
    assert links == {(6, "a1b2c3"), (9, "d4e5f6")}
    assert p.decode_pairs(bytes.fromhex("d4e5f6a1b2c3")) == {"d4e5f6": "a1b2c3"}
    plug = p.decode_plugins(bytes([3, 2, 2]))
    assert plug.ok(p.PLUGIN_ROOM) and not plug.ok(p.PLUGIN_GAS)
    assert p.decode_plugins(bytes([3])).ok(p.PLUGIN_GAS)        # old firmware: one byte


def test_snap_follows_direction():
    assert p.snap(47.6, p.ONDOL_TEMPS, 45) == 48
    assert p.snap(62, p.ONDOL_TEMPS, 60) == 65      # going up from 60 skips to the next allowed value
    assert p.snap(62, p.ONDOL_TEMPS, 65) == 60      # going down
    assert p.snap(57, p.HOT_WATER_TEMPS) == 55      # no direction: nearest
    assert p.snap(48, p.HOT_WATER_TEMPS, 47) == 50  # the boiler takes 35-47, 50, 55 and 60 only
    assert p.snap(54, p.HOT_WATER_TEMPS, 55) == 50


def test_requests_and_lines():
    assert p.boiler_cmd("a1b2c3", "b1", "1") == "f06@a1b2c3 b1=1"
    assert p.zone_cmd("d4e5f6", "z3", "d4=17") == "f09@d4e5f6 a0=7,d4=17"
    assert p.encode_line(0, b"refresh") == b"00 72656672657368\n"
    assert p.parse_line("01 06a1b2c3\n") == (1, bytes.fromhex("06a1b2c3"))
    assert p.parse_line("0a 00") == (0x0A, b"\x00")
    assert p.parse_line("ping") is None and p.parse_line("zz 00") is None


def test_model_code_picks_the_tables():
    ours = p.tables_for("23")                       # our boiler: the measured tables
    assert ours.heat == p.ONDOL_TEMPS and ours.hot_water == p.HOT_WATER_TEMPS and ours.hot_water_step == 1
    assert p.tables_for(None) == ours               # older hub firmware: no model code
    t = p.tables_for("06")
    assert t.heat[0] == 35 and t.hot_water[0] == 30 and 37.5 in t.hot_water and t.hot_water_step == 0.5
    assert p.tables_for("03").hot_water[:4] == [35, 36, 37, 37.5]
    assert p.tables_for("07").hot_water == [1, 2, 3] and p.tables_for("07").hot_water_steps   # unknown code: 3 steps, as the app
    assert p.tables_for("03", p.KIND_THERMOSTAT).hot_water == p.HOT_WATER_TEMPS              # our thermostat controller
    assert p.tables_for("41", p.KIND_THERMOSTAT).heat[2] == 47                               # 13-step heating water
    assert p.tables_for("41", p.KIND_THERMOSTAT).hot_water == list(range(1, 8))
    assert p.tables_for("ff", p.KIND_THERMOSTAT).heat is None                                # hot water only
    payload = bytes([6]) + bytes.fromhex("a1b2c3") + b"\x23" + bytes([9]) + bytes.fromhex("d4e5f6") + b"\x03" + b"\x06"
    assert p.decode_models(payload) == {(6, "a1b2c3"): "23", (9, "d4e5f6"): "03"}


def test_labels_decode():
    # ([kind][mac3][len][utf8]) * n — thermostats only (f06 ignored), variable length
    one = bytes([9]) + bytes.fromhex("d4e5f6") + bytes([len("1층".encode())]) + "1층".encode()
    two = bytes([9]) + bytes.fromhex("a1b2c3") + bytes([len("2층".encode())]) + "2층".encode()
    boiler = bytes([6]) + bytes.fromhex("010203") + bytes([len("x".encode())]) + b"x"
    assert p.decode_labels(one + two + boiler) == {"d4e5f6": "1층", "a1b2c3": "2층"}
    assert p.decode_labels(b"") == {}
    assert p.decode_labels(bytes([9]) + bytes.fromhex("d4e5f6") + b"\x00") == {}   # empty label skipped
    # truncated entry (len says more than remains) stops cleanly
    assert p.decode_labels(bytes([9]) + bytes.fromhex("d4e5f6") + b"\x05ab") == {}


def test_bill_decode():
    # [basis 1 meter|2 boiler][litres u32][won u32]; [0] or short = unknown
    import struct
    assert p.decode_bill(bytes([2]) + struct.pack("<II", 37000, 48720)) == p.BillState("boiler", 37.0, 48720)
    assert p.decode_bill(bytes([1]) + struct.pack("<II", 123456, 160300)) == p.BillState("meter", 123.46, 160300)
    assert p.decode_bill(bytes([0])) is None
    assert p.decode_bill(bytes([2]) + b"\x00\x00") is None
    assert p.decode_bill(bytes([3]) + struct.pack("<II", 1, 1)) is None


def test_half_degree_hot_water():
    raw = list(F06_ON)
    raw[10:13] = "255"                              # 0x25 = 37 and tenths 5
    assert p.decode_boiler(bytes.fromhex("".join(raw))).hot_water_setpoint == 37.5
    half = p.tables_for("03").hot_water
    assert p.snap(37.4, half, 36) == 37.5
    assert p.snap(43.5, half, 60) == 43             # not in the table: step down
    assert p.hot_water_value(37.5) == "25,df=5" and p.hot_water_value(45) == "2D,df=0"
