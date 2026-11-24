"""The boiler's hot water as a water heater entity."""

from __future__ import annotations

from typing import Any

from homeassistant.components.water_heater import STATE_GAS, WaterHeaterEntity, WaterHeaterEntityFeature
from homeassistant.const import ATTR_TEMPERATURE, STATE_OFF, UnitOfTemperature
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LynnaiConfigEntry, protocol as p
from .entity import BoilerEntity, setup_dynamic


async def async_setup_entry(hass: HomeAssistant, entry: LynnaiConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    hub = entry.runtime_data
    entry.async_on_unload(
        setup_dynamic(hass, entry.entry_id, async_add_entities, lambda: [HotWater(hass, hub, entry.entry_id, mac) for mac in hub.boilers])
    )


class HotWater(BoilerEntity, WaterHeaterEntity):
    _attr_temperature_unit = UnitOfTemperature.CELSIUS
    _attr_operation_list = [STATE_OFF, STATE_GAS]
    _attr_supported_features = (
        WaterHeaterEntityFeature.TARGET_TEMPERATURE
        | WaterHeaterEntityFeature.OPERATION_MODE
        | WaterHeaterEntityFeature.ON_OFF
    )

    def __init__(self, hass: HomeAssistant, hub, entry_id: str, mac: str) -> None:
        super().__init__(hass, hub, entry_id, mac, "hot_water")

    # the boiler model picks the table (35-60 by 1, 30-60 / 35-60 with half degrees, or steps 1-3 / 1-7)
    @property
    def min_temp(self) -> float:
        return self.hub.boiler_tables(self.mac).hot_water[0]

    @property
    def max_temp(self) -> float:
        return self.hub.boiler_tables(self.mac).hot_water[-1]

    @property
    def target_temperature_step(self) -> float:
        return self.hub.boiler_tables(self.mac).hot_water_step

    @property
    def precision(self) -> float:
        return self.hub.boiler_tables(self.mac).hot_water_step

    @property
    def current_operation(self) -> str | None:
        state = self.state_or_none
        if state is None:
            return None
        return STATE_GAS if state.hotwater else STATE_OFF

    @property
    def target_temperature(self) -> float | None:
        state = self.state_or_none
        return state.hot_water_setpoint if state else None

    async def async_set_temperature(self, **kwargs: Any) -> None:
        if (temperature := kwargs.get(ATTR_TEMPERATURE)) is not None:
            await self.hub.set_hot_water_setpoint(self.mac, temperature)

    async def async_set_operation_mode(self, operation_mode: str) -> None:
        await self.hub.set_boiler_switch(self.mac, "hotwater", operation_mode != STATE_OFF)

    async def async_turn_on(self, **kwargs: Any) -> None:
        await self.hub.set_boiler_switch(self.mac, "hotwater", True)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self.hub.set_boiler_switch(self.mac, "hotwater", False)
