"""Rooms of a thermostat controller as climate entities."""

from __future__ import annotations

from typing import Any

from homeassistant.components.climate import (
    PRESET_AWAY,
    PRESET_NONE,
    ClimateEntity,
    ClimateEntityFeature,
    HVACAction,
    HVACMode,
)
from homeassistant.const import ATTR_TEMPERATURE, UnitOfTemperature
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LynnaiConfigEntry, protocol as p
from .entity import ZoneEntity, setup_dynamic


async def async_setup_entry(hass: HomeAssistant, entry: LynnaiConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    hub = entry.runtime_data
    entry.async_on_unload(
        setup_dynamic(
            hass,
            entry.entry_id,
            async_add_entities,
            lambda: [RoomClimate(hass, hub, entry.entry_id, mac, z) for mac, zones in hub.zones.items() for z in zones],
        )
    )


class RoomClimate(ZoneEntity, ClimateEntity):
    _attr_name = None           # the entity is the device's main feature
    _attr_hvac_modes = [HVACMode.OFF, HVACMode.HEAT]
    _attr_preset_modes = [PRESET_NONE, PRESET_AWAY]
    _attr_supported_features = (
        ClimateEntityFeature.TARGET_TEMPERATURE
        | ClimateEntityFeature.PRESET_MODE
        | ClimateEntityFeature.TURN_ON
        | ClimateEntityFeature.TURN_OFF
    )
    _attr_temperature_unit = UnitOfTemperature.CELSIUS
    _attr_target_temperature_step = 1
    _attr_min_temp = p.ZONE_MIN_TEMP
    _attr_max_temp = p.ZONE_MAX_TEMP
    _enable_turn_on_off_backwards_compatibility = False

    def __init__(self, hass: HomeAssistant, hub, entry_id: str, mac: str, zone_id: str) -> None:
        super().__init__(hass, hub, entry_id, mac, zone_id, "room")

    def _heating_possible(self) -> bool:
        """A room only heats while its boiler is on with heating enabled."""
        bmac = self.hub.boiler_of(self.mac)
        boiler = self.hub.boilers.get(bmac) if bmac else None
        return boiler is None or (boiler.power and boiler.heat)

    @property
    def hvac_mode(self) -> HVACMode | None:
        zone = self.zone
        if zone is None:
            return None
        return HVACMode.HEAT if zone.on and self._heating_possible() else HVACMode.OFF

    @property
    def hvac_action(self) -> HVACAction | None:
        zone = self.zone
        if zone is None:
            return None
        if not zone.on or not self._heating_possible():
            return HVACAction.OFF
        return HVACAction.HEATING if zone.heating else HVACAction.IDLE

    @property
    def current_temperature(self) -> float | None:
        return self.zone.current if self.zone else None

    @property
    def target_temperature(self) -> float | None:
        return self.zone.target if self.zone else None

    @property
    def preset_mode(self) -> str | None:
        return (PRESET_AWAY if self.zone.away else PRESET_NONE) if self.zone else None

    async def async_set_hvac_mode(self, hvac_mode: HVACMode) -> None:
        await self.hub.set_zone_power(self.mac, self.zone_id, hvac_mode == HVACMode.HEAT)

    async def async_turn_on(self) -> None:
        await self.hub.set_zone_power(self.mac, self.zone_id, True)

    async def async_turn_off(self) -> None:
        await self.hub.set_zone_power(self.mac, self.zone_id, False)

    async def async_set_temperature(self, **kwargs: Any) -> None:
        if (temperature := kwargs.get(ATTR_TEMPERATURE)) is not None:
            await self.hub.set_zone_target(self.mac, self.zone_id, temperature)

    async def async_set_preset_mode(self, preset_mode: str) -> None:
        await self.hub.set_zone_away(self.mac, self.zone_id, preset_mode == PRESET_AWAY)
