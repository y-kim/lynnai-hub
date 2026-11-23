"""Shared pieces for LYNNAI Hub entities."""

from __future__ import annotations

import dataclasses
from collections.abc import Callable, Iterable
from typing import Any

from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity import Entity
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import protocol as p
from .const import DOMAIN, MANUFACTURER, SIGNAL_NEW, SIGNAL_UPDATE
from .hub import LynnaiHub

# Device names cannot be translated by Home Assistant, so pick by the configured language once.
_NAMES = {
    "ko": {"boiler": "보일러", "room": "방{}", "living": "거실", "room_sensor": "온습도계", "gas": "가스 계량기"},
    "en": {"boiler": "Boiler", "room": "Room {}", "living": "Living room", "room_sensor": "Room sensor", "gas": "Gas meter"},
}


def _links_by_device_id() -> bool:
    """Whether this Home Assistant links a device to its parent by registry id.

    2026.8 turned DeviceInfo into a dataclass with `via_device_id` and deprecated `via_device` (the identifier pair).
    The old field is not merely a warning there: when Home Assistant re-adds an entity on behalf of one of its own
    components (renaming it after an area is assigned, say) the deprecation is raised as an error and the entity is lost.
    """
    if dataclasses.is_dataclass(DeviceInfo):
        return any(f.name == "via_device_id" for f in dataclasses.fields(DeviceInfo))
    return "via_device_id" in getattr(DeviceInfo, "__optional_keys__", ())


_VIA_DEVICE_ID = _links_by_device_id()


def _names(hass: HomeAssistant) -> dict[str, str]:
    return _NAMES["ko"] if (hass.config.language or "").startswith("ko") else _NAMES["en"]


def zone_name(hass: HomeAssistant, zone_id: str) -> str:
    number = p.ZONE_SLOTS[zone_id].number
    names = _names(hass)
    return names["living"] if number == 7 else names["room"].format(number)


class LynnaiEntity(Entity):
    """An entity that mirrors part of the hub's state; the hub pushes changes."""

    _attr_has_entity_name = True
    _attr_should_poll = False

    def __init__(self, hub: LynnaiHub, entry_id: str, device_key: str, key: str) -> None:
        self.hub = hub
        self._entry_id = entry_id
        self._attr_unique_id = f"{hub.hub_id}_{device_key}_{key}"
        self._attr_translation_key = key
        self._device_id = (DOMAIN, f"{hub.hub_id}_{device_key}")

    def _device(self, name: str, model: str) -> DeviceInfo:
        info: dict[str, Any] = {
            "identifiers": {self._device_id},
            "manufacturer": MANUFACTURER,
            "model": model,
            "name": name,
        }
        if not _VIA_DEVICE_ID:
            info["via_device"] = (DOMAIN, self.hub.hub_id)
        elif self.hub.device_id:
            info["via_device_id"] = self.hub.device_id
        return DeviceInfo(**info)

    async def async_added_to_hass(self) -> None:
        self.async_on_remove(
            async_dispatcher_connect(self.hass, SIGNAL_UPDATE.format(self._entry_id), self._on_hub_update)
        )

    @callback
    def _on_hub_update(self) -> None:
        self.async_write_ha_state()


class BoilerEntity(LynnaiEntity):
    def __init__(self, hass: HomeAssistant, hub: LynnaiHub, entry_id: str, mac: str, key: str) -> None:
        super().__init__(hub, entry_id, f"b{mac}", key)
        self.mac = mac
        self._attr_device_info = self._device(f"{_names(hass)['boiler']} {mac}", "Boiler controller")

    @property
    def state_or_none(self) -> p.BoilerState | None:
        return self.hub.boilers.get(self.mac)

    @property
    def available(self) -> bool:
        return self.mac in self.hub.boilers and self.hub.device_online(p.KIND_BOILER, self.mac)


class ZoneEntity(LynnaiEntity):
    def __init__(self, hass: HomeAssistant, hub: LynnaiHub, entry_id: str, mac: str, zone_id: str, key: str) -> None:
        super().__init__(hub, entry_id, f"t{mac}_{zone_id}", key)
        self.mac = mac
        self.zone_id = zone_id
        self._attr_device_info = self._device(zone_name(hass, zone_id), "Room thermostat")

    @property
    def zone(self) -> p.ZoneState | None:
        return self.hub.zones.get(self.mac, {}).get(self.zone_id)

    @property
    def available(self) -> bool:
        return self.zone is not None and self.hub.device_online(p.KIND_THERMOSTAT, self.mac)


class HubEntity(LynnaiEntity):
    """Something about the hub itself; it lives on the hub device."""

    def __init__(self, hub: LynnaiHub, entry_id: str, key: str) -> None:
        super().__init__(hub, entry_id, "hub", key)
        self._attr_device_info = DeviceInfo(identifiers={(DOMAIN, hub.hub_id)})


class HubModeEntity(HubEntity):
    """The hub's operating mode and what goes with it. Firmware before v0.6 does not report it."""

    @property
    def available(self) -> bool:
        return self.hub.connected and self.hub.mode is not None


class PluginEntity(LynnaiEntity):
    """A sensor wired to the hub itself (room sensor, gas meter)."""

    def __init__(self, hass: HomeAssistant, hub: LynnaiHub, entry_id: str, plugin: str, bit: int, key: str) -> None:
        super().__init__(hub, entry_id, plugin, key)
        self._bit = bit
        model = "Room sensor" if plugin == "room" else "Gas meter"
        self._attr_device_info = self._device(_names(hass)["room_sensor" if plugin == "room" else "gas"], model)

    @property
    def available(self) -> bool:
        # Older firmware never sends the plugin notice; then being connected is all we know.
        known = self.hub.plugins.enabled != 0
        return self.hub.connected and (not known or self.hub.plugins.ok(self._bit))


_GONE: set[str] = set()     # unique ids of entities that removed themselves (see forget_entity)


def forget_entity(unique_id: str | None) -> None:
    """An entity removed itself because it no longer applies; let setup_dynamic add it again if it applies again."""
    if unique_id:
        _GONE.add(unique_id)


def setup_dynamic(
    hass: HomeAssistant,
    entry_id: str,
    async_add_entities: AddEntitiesCallback,
    make: Callable[[], Iterable[LynnaiEntity]],
) -> Callable[[], None]:
    """Add entities for devices as the hub reports them. `make` lists every entity that should exist now."""
    known: set[str] = set()

    @callback
    def add_new() -> None:
        gone = _GONE & known                         # an entity that took itself away may come back
        known.difference_update(gone)
        _GONE.difference_update(gone)
        new = [e for e in make() if e.unique_id not in known]
        known.update(e.unique_id for e in new)
        if new:
            async_add_entities(new)

    add_new()
    return async_dispatcher_connect(hass, SIGNAL_NEW.format(entry_id), add_new)
