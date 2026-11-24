"""The integration inside Home Assistant, talking to the fake hub over a real socket."""

from __future__ import annotations

import asyncio
import struct
from unittest.mock import patch

import pytest
from pytest_homeassistant_custom_component.common import MockConfigEntry

from homeassistant.core import HomeAssistant
from homeassistant.setup import async_setup_component
from homeassistant.util import dt as dt_util

from custom_components.lynnai_hub.const import DOMAIN
from custom_components.lynnai_hub.entity import _VIA_DEVICE_ID

from .conftest import BOILER, F06_OFF, F06_ON, HUB_ID, THERMO, f09

TOKEN = "ab" * 16


@pytest.fixture(autouse=True)
def _sockets(socket_enabled):
    yield


async def _settle(hass: HomeAssistant, done, timeout: float = 5.0) -> None:
    """Wait until `done()` holds (the client and the fake hub run on real sockets)."""
    for _ in range(int(timeout / 0.05)):
        await hass.async_block_till_done()
        if done():
            return
        await asyncio.sleep(0.05)
    raise AssertionError("timed out waiting for the hub's data")


@pytest.fixture
async def entry(hass: HomeAssistant, fake_hub):
    fake_hub.tokens.add(TOKEN)
    fake_hub.seed()
    e = MockConfigEntry(
        domain=DOMAIN, unique_id=HUB_ID, title="LYNNAI Hub",
        data={"host": "127.0.0.1", "port": fake_hub.port, "token": TOKEN, "hub_id": HUB_ID},
    )
    e.add_to_hass(hass)
    with patch("custom_components.lynnai_hub.discover", return_value={}):
        assert await hass.config_entries.async_setup(e.entry_id)
        await _settle(hass, lambda: hass.states.get("sensor.gas_meter_meter_reading") is not None)
        yield e
        await hass.config_entries.async_unload(e.entry_id)
        await hass.async_block_till_done()


async def test_entities_show_the_hub_state(hass: HomeAssistant, entry, fake_hub):
    s = hass.states
    assert s.get(f"binary_sensor.lynnai_hub_{HUB_ID.lower()}_connection").state == "on"

    assert s.get(f"switch.boiler_{BOILER}_power").state == "on"
    assert s.get(f"switch.boiler_{BOILER}_heating").state == "on"
    assert s.get(f"switch.boiler_{BOILER}_away").state == "off"
    assert s.get(f"switch.boiler_{BOILER}_hot_water_preheat").state == "off"
    # with room thermostats the boiler only heats the water: no floor/room control, and the setpoint is the water's
    assert s.get(f"select.boiler_{BOILER}_heating_control") is None
    assert s.get(f"number.boiler_{BOILER}_heating_water_temperature").state == "65"
    assert s.get(f"select.boiler_{BOILER}_hot_water_preheat_auto_off").state == "unlimited"
    hot = s.get(f"water_heater.boiler_{BOILER}_hot_water")
    assert hot.state == "gas" and hot.attributes["temperature"] == 60
    assert s.get(f"binary_sensor.boiler_{BOILER}_heating_burner").state == "off"

    room1 = s.get("climate.room_1")
    assert room1.state == "heat"
    assert (room1.attributes["current_temperature"], room1.attributes["temperature"]) == (20, 23)
    assert room1.attributes["hvac_action"] == "idle" and room1.attributes["preset_mode"] == "none"
    assert s.get("climate.room_2").state == "off"
    assert s.get("climate.living_room").state == "heat"
    assert s.get("switch.living_room_reservation").state == "off"

    assert s.get("sensor.room_sensor_temperature").state == "25.3"
    assert s.get("sensor.room_sensor_humidity").state == "34"
    total = s.get("sensor.gas_meter_meter_reading")
    assert total.state == "15.04" and total.attributes["state_class"] == "total_increasing"
    assert total.attributes["device_class"] == "gas"
    assert s.get("sensor.gas_meter_flow").state == "0.86"
    assert s.get("sensor.gas_meter_last_24_hours").state == "5.19"


async def test_updates_are_pushed(hass: HomeAssistant, entry, fake_hub):
    await fake_hub.push_state(9, THERMO, f09(z1=0x83 | 0x40, z2=0x83 | 0x10))
    await _settle(hass, lambda: hass.states.get("climate.room_1").attributes["hvac_action"] == "heating")
    room2 = hass.states.get("climate.room_2")
    assert room2.state == "heat" and room2.attributes["preset_mode"] == "away"

    # the boiler stops heating: rooms that are 'on' no longer count as heating
    await fake_hub.push_state(6, BOILER, F06_OFF)
    await _settle(hass, lambda: hass.states.get(f"switch.boiler_{BOILER}_power").state == "off")
    assert hass.states.get("climate.room_1").state == "off"

    await fake_hub.push(0x04, struct.pack("<5I", 1505, 0xFFFFFFFF, 220, 5200, 0))
    await _settle(hass, lambda: hass.states.get("sensor.gas_meter_meter_reading").state == "15.05")


async def test_commands_reach_the_hub(hass: HomeAssistant, entry, fake_hub):
    async def call(domain: str, service: str, entity_id: str, **data) -> None:
        await hass.services.async_call(domain, service, {"entity_id": entity_id, **data}, blocking=True)
        await _settle(hass, lambda: bool(fake_hub.requests))

    await call("climate", "set_temperature", "climate.living_room", temperature=25)
    assert fake_hub.requests.pop() == f"f09@{THERMO} a0=7,d4=19"

    await call("climate", "set_preset_mode", "climate.room_1", preset_mode="away")
    assert fake_hub.requests.pop() == f"f09@{THERMO} a0=1,b3=1"

    await call("climate", "set_hvac_mode", "climate.room_2", hvac_mode="heat")     # boiler already on: just the room
    assert fake_hub.requests == [f"f09@{THERMO} a0=2,b1=1"]
    fake_hub.requests.clear()

    await call("switch", "turn_off", f"switch.boiler_{BOILER}_heating")
    assert fake_hub.requests.pop() == f"f06@{BOILER} b1=0"

    await call("switch", "turn_on", f"switch.boiler_{BOILER}_away")               # away is set room by room,
    assert sorted(fake_hub.requests) == [f"f09@{THERMO} a0={n},b3=1" for n in (1, 7)]   # leaving room 2 (off) alone
    fake_hub.requests.clear()

    await call("select", "select_option", f"select.boiler_{BOILER}_hot_water_preheat_auto_off", option="30")
    assert fake_hub.requests.pop() == "hub preoff=30"
    await _settle(hass, lambda: hass.states.get(f"select.boiler_{BOILER}_hot_water_preheat_auto_off").state == "30")

    await call("number", "set_value", f"number.boiler_{BOILER}_heating_water_temperature", value=62)
    assert fake_hub.requests.pop() == f"f06@{BOILER} d2=3C"       # 62 is not allowed; below 65 it steps down to 60

    await call("water_heater", "set_temperature", f"water_heater.boiler_{BOILER}_hot_water", temperature=45)
    assert fake_hub.requests.pop() == f"f06@{BOILER} d1=2D,df=0"

    await hass.services.async_call("number", "set_value", {
        "entity_id": "number.living_room_reservation_hours", "value": 5}, blocking=True)
    await asyncio.sleep(0.1)
    assert not fake_hub.requests                  # reservation is off: only remembered (no accidental start)
    await _settle(hass, lambda: hass.states.get("number.living_room_reservation_hours").state == "5")
    await call("switch", "turn_on", "switch.living_room_reservation")
    assert fake_hub.requests.pop() == f"f09@{THERMO} a0=7,96=1,97=05"     # starts with the remembered length
    await call("climate", "set_temperature", "climate.living_room", temperature=5)
    assert fake_hub.requests.pop() == f"f09@{THERMO} a0=7,d4=05"          # rooms go down to 5 C
    await call("switch", "turn_off", "switch.living_room_reservation")
    assert fake_hub.requests.pop() == f"f09@{THERMO} a0=7,96=0"


async def test_room_on_wakes_the_boiler(hass: HomeAssistant, entry, fake_hub):
    await fake_hub.push_state(6, BOILER, F06_OFF)
    await _settle(hass, lambda: hass.states.get(f"switch.boiler_{BOILER}_power").state == "off")
    await hass.services.async_call("climate", "turn_on", {"entity_id": "climate.room_2"}, blocking=True)
    await _settle(hass, lambda: len(fake_hub.requests) >= 3)
    assert fake_hub.requests == [f"f06@{BOILER} 80=1", f"f06@{BOILER} b1=1", f"f09@{THERMO} a0=2,b1=1"]


async def test_unavailable_when_the_device_or_hub_goes_away(hass: HomeAssistant, entry, fake_hub):
    await fake_hub.push(0x02, bytes([6]) + bytes.fromhex(BOILER))          # the thermostat dropped off the hub
    await _settle(hass, lambda: hass.states.get("climate.room_1").state == "unavailable")
    assert hass.states.get(f"switch.boiler_{BOILER}_power").state == "on"

    no_pulse = "binary_sensor.gas_meter_no_meter_pulses"
    assert hass.states.get(no_pulse).state == "off"
    await fake_hub.push(0x05, bytes([3, 2, 0]))                            # boiler burns but the meter gives no pulses
    await _settle(hass, lambda: hass.states.get(no_pulse).state == "on")
    assert hass.states.get("sensor.gas_meter_flow").state == "0.86"       # only a warning: the values stay
    await fake_hub.push(0x05, bytes([3, 2, 2]))                            # gas meter paused on the hub
    await _settle(hass, lambda: hass.states.get("sensor.gas_meter_flow").state == "unavailable")
    assert hass.states.get(no_pulse).state == "unavailable"
    assert hass.states.get("sensor.room_sensor_temperature").state == "25.3"

    for w in fake_hub.writers:                                             # the hub itself goes away
        w.close()
    await _settle(hass, lambda: hass.states.get(f"switch.boiler_{BOILER}_power").state == "unavailable")
    assert hass.states.get(f"binary_sensor.lynnai_hub_{HUB_ID.lower()}_connection").state == "off"


@pytest.mark.skipif(not _VIA_DEVICE_ID,
                    reason="needs the Home Assistant (2026.8+) where deprecated device info fails on this path")
async def test_entities_survive_a_change_made_in_the_ui(hass: HomeAssistant, entry, fake_hub, hass_ws_client):
    """Renaming an entity (or assigning an area, which renames it on newer versions) makes Home Assistant add the
    entity again from inside its own `config` component. Deprecated device info is an error on that path — it once
    cost every entity of a device as soon as the user picked an area."""
    from homeassistant.helpers import area_registry as ar, device_registry as dr, entity_registry as er

    await async_setup_component(hass, "config", {})
    ws = await hass_ws_client(hass)
    old, new = f"switch.boiler_{BOILER}_power", "switch.kitchen_boiler_power"
    assert hass.states.get(old).state == "on"

    await ws.send_json_auto_id({"type": "config/entity_registry/update", "entity_id": old, "new_entity_id": new})
    assert (await ws.receive_json())["success"]
    await hass.async_block_till_done()
    assert hass.states.get(new).state == "on"          # still there under the new id, and still live

    devices, areas = dr.async_get(hass), ar.async_get(hass)
    by_ident = {i[1]: d for d in dr.async_entries_for_config_entry(devices, entry.entry_id)
                for i in d.identifiers if i[0] == DOMAIN}
    room, hub_device = by_ident[f"{HUB_ID}_t{THERMO}_z1"], by_ident[HUB_ID]
    assert room.via_device_id == hub_device.id         # the rooms hang off the hub device
    await ws.send_json_auto_id({"type": "config/device_registry/update", "device_id": room.id,
                                "area_id": areas.async_create("Bedroom").id})
    assert (await ws.receive_json())["success"]
    await hass.async_block_till_done()
    climates = [e.entity_id for e in er.async_entries_for_device(er.async_get(hass), room.id) if e.domain == "climate"]
    assert len(climates) == 1 and hass.states.get(climates[0]).state == "heat"


async def test_hub_temperature_appears_when_the_hub_sends_it(hass: HomeAssistant, entry, fake_hub):
    """The alive message carries the hub's chip temperature only when it is turned on for this channel."""
    hub = f"lynnai_hub_{HUB_ID.lower()}"
    temp = f"sensor.{hub}_internal_temperature"
    await fake_hub.push(0x0A, bytes([0]))                           # older firmware, or turned off: nothing to show
    await hass.async_block_till_done()
    assert hass.states.get(temp) is None
    await fake_hub.push(0x0A, bytes([0]) + struct.pack("<h", 3715))
    await _settle(hass, lambda: hass.states.get(temp) is not None)
    assert hass.states.get(temp).state in ("37.1", "37.2")          # 37.15 rounds to one decimal either way
    await fake_hub.push(0x0A, bytes([0]) + struct.pack("<h", 4020))
    await _settle(hass, lambda: hass.states.get(temp).state == "40.2")


async def test_hub_mode_can_be_seen_and_changed(hass: HomeAssistant, entry, fake_hub):
    hub = f"lynnai_hub_{HUB_ID.lower()}"
    mode, fallback, auto = f"select.{hub}_operating_mode", f"binary_sensor.{hub}_running_without_the_cloud", \
        f"switch.{hub}_standalone_while_the_cloud_is_unreachable"
    await _settle(hass, lambda: hass.states.get(mode) is not None)
    assert hass.states.get(mode).state == "proxy"
    assert hass.states.get(fallback).state == "off"
    assert hass.states.get(auto).state == "on"

    await hass.services.async_call("select", "select_option", {"entity_id": mode, "option": "standalone"}, blocking=True)
    await _settle(hass, lambda: fake_hub.requests[-1:] == ["hub mode=standalone"])
    await _settle(hass, lambda: hass.states.get(mode).state == "standalone")     # shown once the hub confirms it

    await hass.services.async_call("switch", "turn_off", {"entity_id": auto}, blocking=True)
    await _settle(hass, lambda: fake_hub.requests[-1:] == ["hub fallback=0"])
    await _settle(hass, lambda: hass.states.get(auto).state == "off")

    status = f"sensor.{hub}_status"
    assert hass.states.get(status).state == "Standalone"

    # set to proxy, but the Rinnai server has been unreachable since 18:25 local time: the mode stays what it is set to
    await hass.config.async_set_time_zone("Asia/Seoul")
    since = int(dt_util.now().replace(hour=18, minute=25, second=0, microsecond=0).timestamp())
    await fake_hub.push(0x0B, bytes([0, 1, 1]) + struct.pack("<I", since))
    await _settle(hass, lambda: hass.states.get(fallback).state == "on")
    assert hass.states.get(mode).state == "proxy"
    assert hass.states.get(status).state == "Standalone (Rinnai server unreachable since 18:25)"
    await fake_hub.push(0x0B, bytes([0, 1, 1]))        # an older, shorter message: no time
    await _settle(hass, lambda: hass.states.get(status).state == "Standalone (Rinnai server unreachable)")
    # a newer hub also says what went wrong: [reason][refusal code] after the time
    for reason, text in ((1, "home Wi-Fi down"), (2, "cannot connect to the Rinnai server"),
                         (3, "secure connection to the Rinnai server fails"),
                         (4, "Rinnai server refuses the login (code 5)"),
                         (5, "Rinnai server does not answer the login"), (6, "Rinnai server closes the connection"),
                         (7, "cannot look up the Rinnai server's address")):
        await fake_hub.push(0x0B, bytes([0, 1, 1]) + struct.pack("<I", since) + bytes([reason, 5]))
        await _settle(hass, lambda: hass.states.get(status).state == f"Standalone ({text} since 18:25)")
    assert hass.states.get(status).attributes["reason"] == "dns"
    await fake_hub.push(0x0B, bytes([0, 1, 1]) + struct.pack("<I", since) + bytes([4, 5]))
    await _settle(hass, lambda: hass.states.get(status).attributes.get("refusal_code") == 5)
    await fake_hub.push(0x0B, bytes([0, 0, 1, 0, 0, 0, 0]))
    await _settle(hass, lambda: hass.states.get(status).state == "Proxy")
    assert hass.states.get(fallback).state == "off"


async def test_no_mode_entities_for_a_hub_that_does_not_report_it(hass: HomeAssistant, fake_hub):
    """Firmware before the mode message: the hub device keeps just its connection entity."""
    fake_hub.tokens.add(TOKEN)
    fake_hub.seed()
    fake_hub.on_hello = [m for m in fake_hub.on_hello if m[0] != 0x0B]
    e = MockConfigEntry(domain=DOMAIN, unique_id=HUB_ID, title="LYNNAI Hub",
                        data={"host": "127.0.0.1", "port": fake_hub.port, "token": TOKEN, "hub_id": HUB_ID})
    e.add_to_hass(hass)
    with patch("custom_components.lynnai_hub.discover", return_value={}):
        assert await hass.config_entries.async_setup(e.entry_id)
        await _settle(hass, lambda: hass.states.get("sensor.gas_meter_meter_reading") is not None)
        assert hass.states.get(f"select.lynnai_hub_{HUB_ID.lower()}_operating_mode") is None
        await hass.config_entries.async_unload(e.entry_id)
        await hass.async_block_till_done()


@pytest.fixture
async def boiler_only(hass: HomeAssistant, fake_hub):
    fake_hub.tokens.add(TOKEN)
    fake_hub.seed(thermostat=False)
    e = MockConfigEntry(
        domain=DOMAIN, unique_id=HUB_ID, title="LYNNAI Hub",
        data={"host": "127.0.0.1", "port": fake_hub.port, "token": TOKEN, "hub_id": HUB_ID},
    )
    e.add_to_hass(hass)
    with patch("custom_components.lynnai_hub.discover", return_value={}):
        assert await hass.config_entries.async_setup(e.entry_id)
        await _settle(hass, lambda: hass.states.get("sensor.gas_meter_meter_reading") is not None)
        yield e
        await hass.config_entries.async_unload(e.entry_id)
        await hass.async_block_till_done()


async def test_floor_or_room_control_only_without_room_thermostats(hass: HomeAssistant, boiler_only, fake_hub):
    s = hass.states
    assert s.get(f"select.boiler_{BOILER}_heating_control").state == "ondol"
    assert s.get(f"number.boiler_{BOILER}_heating_temperature").state == "65"
    await hass.services.async_call("select", "select_option", {
        "entity_id": f"select.boiler_{BOILER}_heating_control", "option": "room"}, blocking=True)
    await _settle(hass, lambda: bool(fake_hub.requests))
    assert fake_hub.requests.pop() == f"f06@{BOILER} b4=1"

    # a room thermostat joins the hub: the boiler now only heats the water, so floor/room control goes away
    await fake_hub.push(0x02, bytes([6]) + bytes.fromhex(BOILER) + bytes([9]) + bytes.fromhex(THERMO))
    await fake_hub.push_state(9, THERMO, f09())
    await _settle(hass, lambda: s.get(f"select.boiler_{BOILER}_heating_control") is None)



async def test_boiler_mode_set_on_the_hub(hass: HomeAssistant, boiler_only, fake_hub):
    s = hass.states
    sel = f"select.boiler_{BOILER}_heating_control"
    assert s.get(sel) is not None                                     # boiler alone, nothing set: floor/room
    await fake_hub.push(0x0C, bytes([0, 1]))                          # set on the hub: heating water
    await _settle(hass, lambda: s.get(sel) is None)
    await fake_hub.push_state(6, BOILER, F06_ON[:7] + "1" + F06_ON[8:])   # the boiler says it runs in room mode
    await _settle(hass, lambda: s.get(sel) is not None and s.get(sel).state == "room")
    await fake_hub.push_state(6, BOILER, F06_ON)
    await _settle(hass, lambda: s.get(sel) is None)
    await fake_hub.push(0x0C, bytes([0, 2]))                          # floor/room
    await _settle(hass, lambda: s.get(sel) is not None)


async def test_time_mode_and_preheat_time_left(hass: HomeAssistant, entry, fake_hub):
    s = hass.states
    assert s.get("switch.room_1_timed_heating").state == "off"

    async def press(domain, service, entity_id, **data):
        fake_hub.requests.clear()
        await hass.services.async_call(domain, service, {"entity_id": entity_id, **data}, blocking=True)
        await _settle(hass, lambda: bool(fake_hub.requests))
        await asyncio.sleep(0.05)
        return list(fake_hub.requests)

    # room 1 is on (temperature): time mode is only b4=1
    assert await press("switch", "turn_on", "switch.room_1_timed_heating") == [f"f09@{THERMO} a0=1,b4=1"]
    blob = bytearray(bytes.fromhex(f09()))
    blob[0] |= 0x04                                                   # the thermostat reports time mode
    await fake_hub.push_state(9, THERMO, blob.hex())
    await _settle(hass, lambda: s.get("switch.room_1_timed_heating").state == "on")
    assert s.get("climate.room_1").state == "off"                     # one of the two is on
    assert await press("climate", "turn_on", "climate.room_1") == [f"f09@{THERMO} a0=1,b4=0"]
    assert await press("switch", "turn_off", "switch.room_1_timed_heating") == [f"f09@{THERMO} a0=1,b1=0"]

    sel = f"select.boiler_{BOILER}_hot_water_preheat_auto_off"
    await fake_hub.push(0x0C, bytes([20, 0, 14]))                     # preheat on, 14 minutes left
    await _settle(hass, lambda: s.get(sel).state == "14")
    await fake_hub.push(0x0C, bytes([20, 0, 0]))                      # off again: the default
    await _settle(hass, lambda: s.get(sel).state == "20")
    assert await press("select", "select_option", sel, option="7") == ["hub preoff=7"]


async def test_time_mode_minutes(hass: HomeAssistant, entry, fake_hub):
    """Minutes per hour (f09 da): remembered while the room is not in time mode, sent when the time switch goes on."""
    s = hass.states
    sel = "select.room_1_timed_heating_minutes_per_hour"
    assert s.get(sel).state == "10"                                   # unknown (0) -> 10

    async def press(domain, service, entity_id, **data):
        fake_hub.requests.clear()
        await hass.services.async_call(domain, service, {"entity_id": entity_id, **data}, blocking=True)
        await _settle(hass, lambda: bool(fake_hub.requests))
        await asyncio.sleep(0.05)
        return list(fake_hub.requests)

    fake_hub.requests.clear()
    await hass.services.async_call("select", "select_option", {"entity_id": sel, "option": "30"}, blocking=True)
    await asyncio.sleep(0.2)
    assert fake_hub.requests == []                                    # not in time mode: only remembered
    await _settle(hass, lambda: s.get(sel).state == "30")
    assert await press("switch", "turn_on", "switch.room_1_timed_heating") == [
        f"f09@{THERMO} a0=1,b4=1", f"f09@{THERMO} a0=1,da=1e"]
    blob = bytearray(bytes.fromhex(f09()))
    blob[0] |= 0x04
    blob[5] = 30
    await fake_hub.push_state(9, THERMO, blob.hex())
    await _settle(hass, lambda: s.get("switch.room_1_timed_heating").state == "on")
    assert s.get(sel).state == "30"
    assert await press("select", "select_option", sel, option="5") == [f"f09@{THERMO} a0=1,da=05"]
    blob[5] = 5
    await fake_hub.push_state(9, THERMO, blob.hex())
    await _settle(hass, lambda: s.get(sel).state == "5")
