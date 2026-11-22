"""How the boiler regulates heating: by floor water temperature or by room temperature."""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LynnaiConfigEntry, protocol as p
from .entity import BoilerEntity, setup_dynamic


async def async_setup_entry(hass: HomeAssistant, entry: LynnaiConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    hub = entry.runtime_data
    entry.async_on_unload(
        setup_dynamic(hass, entry.entry_id, async_add_entities, lambda: [HeatMode(hass, hub, entry.entry_id, mac) for mac in hub.boilers])
    )


class HeatMode(BoilerEntity, SelectEntity):
    _attr_options = [p.HEAT_MODE_ONDOL, p.HEAT_MODE_ROOM]

    def __init__(self, hass: HomeAssistant, hub, entry_id: str, mac: str) -> None:
        super().__init__(hass, hub, entry_id, mac, "heat_mode")

    @property
    def current_option(self) -> str | None:
        state = self.state_or_none
        return state.heat_mode if state else None

    async def async_select_option(self, option: str) -> None:
        await self.hub.set_boiler_heat_mode(self.mac, option)
