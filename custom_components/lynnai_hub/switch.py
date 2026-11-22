"""Boiler switches, the per-room reservation switch, and the hub's automatic fallback."""

from __future__ import annotations

from typing import Any

from homeassistant.components.switch import SwitchEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LynnaiConfigEntry
from .entity import BoilerEntity, HubModeEntity, ZoneEntity, setup_dynamic

BOILER_SWITCHES = ("power", "heat", "away", "hotwater", "preheat")


async def async_setup_entry(hass: HomeAssistant, entry: LynnaiConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    hub = entry.runtime_data

    def make():
        out = [BoilerSwitch(hass, hub, entry.entry_id, mac, name) for mac in hub.boilers for name in BOILER_SWITCHES]
        out += [ReservationSwitch(hass, hub, entry.entry_id, mac, z) for mac, zones in hub.zones.items() for z in zones]
        if hub.mode is not None:
            out.append(AutoFallbackSwitch(hub, entry.entry_id))
        return out

    entry.async_on_unload(setup_dynamic(hass, entry.entry_id, async_add_entities, make))


class BoilerSwitch(BoilerEntity, SwitchEntity):
    def __init__(self, hass: HomeAssistant, hub, entry_id: str, mac: str, name: str) -> None:
        super().__init__(hass, hub, entry_id, mac, name)
        self._name = name

    @property
    def is_on(self) -> bool | None:
        state = self.state_or_none
        if state is None:
            return None
        away = self.hub.boiler_away(self.mac)
        if self._name == "away":
            return away
        if self._name == "heat":            # shown off while every room is away, like the vendor's own integration
            return state.heat and not away
        return getattr(state, self._name)

    async def _set(self, on: bool) -> None:
        if self._name == "heat":
            await self.hub.set_boiler_heating(self.mac, on)
        elif self._name == "away":
            await self.hub.set_boiler_away(self.mac, on)
        else:
            await self.hub.set_boiler_switch(self.mac, self._name, on)

    async def async_turn_on(self, **kwargs: Any) -> None:
        await self._set(True)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self._set(False)


class ReservationSwitch(ZoneEntity, SwitchEntity):
    """Run the room for a number of hours (set with the 'reservation hours' number), then stop."""

    def __init__(self, hass: HomeAssistant, hub, entry_id: str, mac: str, zone_id: str) -> None:
        super().__init__(hass, hub, entry_id, mac, zone_id, "reservation")

    @property
    def is_on(self) -> bool | None:
        return self.zone.reserved if self.zone else None

    async def async_turn_on(self, **kwargs: Any) -> None:
        zone = self.zone
        hours = zone.reserve_hours if zone and 1 <= zone.reserve_hours <= 24 else 1
        await self.hub.set_zone_reservation(self.mac, self.zone_id, hours)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self.hub.set_zone_reservation(self.mac, self.zone_id, None)


class AutoFallbackSwitch(HubModeEntity, SwitchEntity):
    """In proxy mode: serve the devices without the cloud while the Rinnai server cannot be reached."""

    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, hub, entry_id: str) -> None:
        super().__init__(hub, entry_id, "auto_fallback")

    @property
    def is_on(self) -> bool | None:
        return self.hub.mode.auto_fallback if self.hub.mode else None

    async def async_turn_on(self, **kwargs: Any) -> None:
        await self.hub.set_auto_fallback(True)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self.hub.set_auto_fallback(False)
