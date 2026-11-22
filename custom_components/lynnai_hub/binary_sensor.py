"""Burner state of a boiler, and whether the hub is connected."""

from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LynnaiConfigEntry
from .const import DOMAIN
from .entity import BoilerEntity, LynnaiEntity, setup_dynamic

BURNERS = {"heat_burning": "heat_burning", "hot_water_burning": "hot_water_burning"}


async def async_setup_entry(hass: HomeAssistant, entry: LynnaiConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    hub = entry.runtime_data
    async_add_entities([HubConnected(hub, entry.entry_id)])
    entry.async_on_unload(
        setup_dynamic(
            hass,
            entry.entry_id,
            async_add_entities,
            lambda: [Burner(hass, hub, entry.entry_id, mac, key) for mac in hub.boilers for key in BURNERS],
        )
    )


class Burner(BoilerEntity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.RUNNING

    def __init__(self, hass: HomeAssistant, hub, entry_id: str, mac: str, key: str) -> None:
        super().__init__(hass, hub, entry_id, mac, key)
        self._field = BURNERS[key]

    @property
    def is_on(self) -> bool | None:
        state = self.state_or_none
        return getattr(state, self._field) if state else None


class HubConnected(LynnaiEntity, BinarySensorEntity):
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, hub, entry_id: str) -> None:
        super().__init__(hub, entry_id, "hub", "connected")
        self._attr_device_info = DeviceInfo(identifiers={(DOMAIN, hub.hub_id)})

    @property
    def is_on(self) -> bool:
        return self.hub.connected

    @property
    def extra_state_attributes(self) -> dict[str, str] | None:
        return {"problem": self.hub.problem} if self.hub.problem and not self.hub.connected else None
