"""The hub object's bookkeeping without Home Assistant entities: pulse size, resets, limits, plugin notice."""

from __future__ import annotations

import struct

from custom_components.lynnai_hub import protocol as p
from custom_components.lynnai_hub.hub import MAX_DEVICES, LynnaiHub

from .conftest import F06_ON


def _gas(pulses: int, reading_l: int, step_l: int | None = None) -> bytes:
    b = struct.pack("<5I", pulses, reading_l, 0, 0, 0)
    return b + (struct.pack("<I", step_l) if step_l is not None else b"")


def test_pulse_size_from_the_hub():
    assert p.decode_gas(_gas(100, 0xFFFFFFFF, 100)).total_m3 == 10.0      # 100 L pulses
    assert p.decode_gas(_gas(100, 0xFFFFFFFF)).total_m3 == 1.0            # older firmware: 10 L


def test_meter_reading_set_starts_a_new_cycle():
    hub = LynnaiHub("X")
    hub.handle_message(p.CMD_GAS, _gas(100, 15040, 10))
    hub.handle_message(p.CMD_GAS, _gas(101, 15050, 10))                   # one pulse of real use
    assert hub.gas_reset is None
    hub.handle_message(p.CMD_GAS, _gas(101, 1694000, 10))                 # reading set on the hub: a jump
    assert hub.gas_reset is not None


def test_new_billing_period_resets():
    hub = LynnaiHub("X")
    hub.handle_message(p.CMD_BILL, bytes([1]) + struct.pack("<II", 30000, 33000))
    assert hub.bill_reset is None
    hub.handle_message(p.CMD_BILL, bytes([1]) + struct.pack("<II", 200, 250))
    assert hub.bill_reset is not None


def test_device_limit():
    hub = LynnaiHub("X")
    for i in range(MAX_DEVICES + 5):
        hub.handle_message(p.CMD_STATE, bytes([6]) + i.to_bytes(3, "big") + bytes.fromhex(F06_ON))
    assert len(hub.boilers) == MAX_DEVICES


def test_plugins_all_off_is_known():
    hub = LynnaiHub("X")
    assert not hub.plugins_seen
    hub.handle_message(p.CMD_PLUGINS, bytes([0]))
    assert hub.plugins_seen and hub.plugins.enabled == 0
