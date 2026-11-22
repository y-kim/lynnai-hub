"""Selects: how the boiler regulates heating, and the hub's operating mode."""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LynnaiConfigEntry, protocol as p
from .entity import BoilerEntity, HubModeEntity, setup_dynamic


async def async_setup_entry(hass: HomeAssistant, entry: LynnaiConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    hub = entry.runtime_data

    def make():
        out: list = [HeatMode(hass, hub, entry.entry_id, mac) for mac in hub.boilers]
        if hub.mode is not None:
            out.append(HubModeSelect(hub, entry.entry_id))
        return out

    entry.async_on_unload(setup_dynamic(hass, entry.entry_id, async_add_entities, make))


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


class HubModeSelect(HubModeEntity, SelectEntity):
    """Proxy (relay to the Rinnai cloud, the Rinnai app keeps working) or standalone (no cloud)."""

    _attr_options = [p.HUB_MODE_PROXY, p.HUB_MODE_STANDALONE]
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, hub, entry_id: str) -> None:
        super().__init__(hub, entry_id, "hub_mode")

    @property
    def current_option(self) -> str | None:
        return self.hub.mode.mode if self.hub.mode else None

    async def async_select_option(self, option: str) -> None:
        await self.hub.set_hub_mode(option)
