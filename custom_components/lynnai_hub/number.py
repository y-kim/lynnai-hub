"""Boiler heating setpoint and the per-room reservation length."""

from __future__ import annotations

from homeassistant.components.number import NumberDeviceClass, NumberEntity, NumberMode
from homeassistant.const import UnitOfTemperature, UnitOfTime
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LynnaiConfigEntry, protocol as p
from .entity import BoilerEntity, ZoneEntity, setup_dynamic


async def async_setup_entry(hass: HomeAssistant, entry: LynnaiConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    hub = entry.runtime_data

    def make():
        out = [HeatSetpoint(hass, hub, entry.entry_id, mac) for mac in hub.boilers]
        out += [ReservationHours(hass, hub, entry.entry_id, mac, z) for mac, zones in hub.zones.items() for z in zones]
        return out

    entry.async_on_unload(setup_dynamic(hass, entry.entry_id, async_add_entities, make))


class HeatSetpoint(BoilerEntity, NumberEntity):
    """Heating water temperature (floor mode, 40-85) or room temperature (room mode, 5-40)."""

    _attr_device_class = NumberDeviceClass.TEMPERATURE
    _attr_native_unit_of_measurement = UnitOfTemperature.CELSIUS
    _attr_native_step = 1
    _attr_mode = NumberMode.BOX

    def __init__(self, hass: HomeAssistant, hub, entry_id: str, mac: str) -> None:
        super().__init__(hass, hub, entry_id, mac, "heat_setpoint")

    def _allowed(self) -> list[int]:
        state = self.state_or_none
        return p.ROOM_TEMPS if state and state.heat_mode == p.HEAT_MODE_ROOM else p.ONDOL_TEMPS

    @property
    def native_min_value(self) -> float:
        return self._allowed()[0]

    @property
    def native_max_value(self) -> float:
        return self._allowed()[-1]

    @property
    def native_value(self) -> float | None:
        state = self.state_or_none
        return state.heat_setpoint if state else None

    async def async_set_native_value(self, value: float) -> None:
        await self.hub.set_boiler_heat_setpoint(self.mac, value)


class ReservationHours(ZoneEntity, NumberEntity):
    """Setting this starts a reservation of that many hours (the device has no 'length only' command)."""

    _attr_native_min_value = 1
    _attr_native_max_value = 24
    _attr_native_step = 1
    _attr_native_unit_of_measurement = UnitOfTime.HOURS
    _attr_mode = NumberMode.SLIDER

    def __init__(self, hass: HomeAssistant, hub, entry_id: str, mac: str, zone_id: str) -> None:
        super().__init__(hass, hub, entry_id, mac, zone_id, "reservation_hours")
        self._last = 1

    @property
    def native_value(self) -> float | None:
        zone = self.zone
        if zone is None:
            return None
        if zone.reserved and 1 <= zone.reserve_hours <= 24:
            self._last = zone.reserve_hours
        return self._last

    async def async_set_native_value(self, value: float) -> None:
        self._last = int(value)
        await self.hub.set_zone_reservation(self.mac, self.zone_id, self._last)
