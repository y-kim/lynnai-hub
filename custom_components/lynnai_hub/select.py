"""Selects: how the boiler regulates heating, hot water preheat auto-off, a room's time-mode minutes and the hub's
operating mode."""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LynnaiConfigEntry, protocol as p
from .entity import BoilerEntity, HubModeEntity, ZoneEntity, forget_entity, setup_dynamic


async def async_setup_entry(hass: HomeAssistant, entry: LynnaiConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    hub = entry.runtime_data

    def make():
        # floor/room control only where the boiler regulates the rooms itself (no room thermostats: see boiler_water_mode)
        out: list = [HeatMode(hass, hub, entry.entry_id, mac) for mac in hub.boilers if not hub.boiler_water_mode(mac)]
        if hub.preheat_off is not None:
            out += [PreheatOff(hass, hub, entry.entry_id, mac) for mac in hub.boilers]
        out += [TimeMinutes(hass, hub, entry.entry_id, mac, z) for mac, zones in hub.zones.items() for z in zones]
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

    @callback
    def _on_hub_update(self) -> None:
        if self.hub.boiler_water_mode(self.mac):     # now heating-water mode (setting, thermostat): take it away
            forget_entity(self.unique_id)            # ...and bring it back if the boiler goes to floor/room mode again
            registry = er.async_get(self.hass)
            if self.registry_entry and registry.async_get(self.entity_id):
                registry.async_remove(self.entity_id)
            else:
                self.hass.async_create_task(self.async_remove(force_remove=True))
            return
        super()._on_hub_update()


class PreheatOff(BoilerEntity, SelectEntity):
    """The hub turns hot water preheat off after this long (it never stops by itself). One setting for the hub.
    Picked while preheat is on, it sets the time left from now for this run; while off, the default for the next one."""

    _attr_options = [p.preheat_off_option(m) for m in p.PREHEAT_OFF_CHOICES]
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, hass: HomeAssistant, hub, entry_id: str, mac: str) -> None:
        super().__init__(hass, hub, entry_id, mac, "preheat_off")

    @property
    def current_option(self) -> str | None:
        """While preheat is on: the minutes left (the hub reports them every minute); otherwise the default."""
        left = self.hub.preheat_left
        if left:
            return p.preheat_off_option(0 if left == 255 else left)
        return None if self.hub.preheat_off is None else p.preheat_off_option(self.hub.preheat_off)

    async def async_select_option(self, option: str) -> None:
        minutes = p.preheat_off_minutes(option)
        if minutes is not None:
            await self.hub.set_preheat_off(minutes)


class TimeMinutes(ZoneEntity, SelectEntity):
    """Time mode: minutes of heating every hour. Sent at once while the room heats in time mode; otherwise only
    remembered and sent when the time switch is turned on."""

    _attr_options = [str(m) for m in p.TIME_MINUTES_CHOICES]

    def __init__(self, hass: HomeAssistant, hub, entry_id: str, mac: str, zone_id: str) -> None:
        super().__init__(hass, hub, entry_id, mac, zone_id, "time_minutes")

    @property
    def current_option(self) -> str | None:
        if self.zone is None:
            return None
        return str(self.hub.zone_time_minutes(self.mac, self.zone_id))

    async def async_select_option(self, option: str) -> None:
        if option.isdigit():
            await self.hub.set_zone_time_minutes(self.mac, self.zone_id, int(option))


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
