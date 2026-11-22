"""Sensors wired to the hub: room temperature/humidity and the gas meter."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity, SensorEntityDescription, SensorStateClass
from homeassistant.const import PERCENTAGE, UnitOfTemperature, UnitOfVolume, UnitOfVolumeFlowRate
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback

from . import LynnaiConfigEntry, protocol as p
from .entity import PluginEntity, setup_dynamic
from .hub import LynnaiHub


@dataclass(frozen=True, kw_only=True)
class LynnaiSensorDescription(SensorEntityDescription):
    plugin: str
    bit: int
    value: Callable[[LynnaiHub], float | None]


SENSORS = (
    LynnaiSensorDescription(
        key="temperature", plugin="room", bit=p.PLUGIN_ROOM,
        device_class=SensorDeviceClass.TEMPERATURE, native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT, suggested_display_precision=1,
        value=lambda hub: hub.room.temperature if hub.room else None,
    ),
    LynnaiSensorDescription(
        key="humidity", plugin="room", bit=p.PLUGIN_ROOM,
        device_class=SensorDeviceClass.HUMIDITY, native_unit_of_measurement=PERCENTAGE,
        state_class=SensorStateClass.MEASUREMENT,
        value=lambda hub: hub.room.humidity if hub.room else None,
    ),
    # The meter total feeds Home Assistant's energy dashboard (gas consumption).
    LynnaiSensorDescription(
        key="gas_total", plugin="gas", bit=p.PLUGIN_GAS,
        device_class=SensorDeviceClass.GAS, native_unit_of_measurement=UnitOfVolume.CUBIC_METERS,
        state_class=SensorStateClass.TOTAL_INCREASING, suggested_display_precision=2,
        value=lambda hub: hub.gas.total_m3 if hub.gas else None,
    ),
    LynnaiSensorDescription(
        key="gas_flow", plugin="gas", bit=p.PLUGIN_GAS,
        device_class=SensorDeviceClass.VOLUME_FLOW_RATE, native_unit_of_measurement=UnitOfVolumeFlowRate.CUBIC_METERS_PER_HOUR,
        state_class=SensorStateClass.MEASUREMENT, suggested_display_precision=2,
        value=lambda hub: hub.gas.flow_m3h if hub.gas else None,
    ),
    # Rolling windows are not meter totals, so they carry no device class (it would demand a 'total' state class).
    LynnaiSensorDescription(
        key="gas_last_day", plugin="gas", bit=p.PLUGIN_GAS,
        native_unit_of_measurement=UnitOfVolume.CUBIC_METERS, state_class=SensorStateClass.MEASUREMENT,
        suggested_display_precision=2,
        value=lambda hub: hub.gas.day_m3 if hub.gas else None,
    ),
    LynnaiSensorDescription(
        key="gas_last_hour", plugin="gas", bit=p.PLUGIN_GAS,
        native_unit_of_measurement=UnitOfVolume.CUBIC_METERS, state_class=SensorStateClass.MEASUREMENT,
        suggested_display_precision=2,
        value=lambda hub: hub.gas.hour_m3 if hub.gas else None,
    ),
)


async def async_setup_entry(hass: HomeAssistant, entry: LynnaiConfigEntry, async_add_entities: AddEntitiesCallback) -> None:
    hub = entry.runtime_data

    def make():
        out = []
        for d in SENSORS:
            source = hub.room if d.plugin == "room" else hub.gas
            if source is None:
                continue
            if d.key == "humidity" and hub.room.humidity is None:    # a sensor without humidity (e.g. MPL3115A2)
                continue
            out.append(HubSensor(hass, hub, entry.entry_id, d))
        return out

    entry.async_on_unload(setup_dynamic(hass, entry.entry_id, async_add_entities, make))


class HubSensor(PluginEntity, SensorEntity):
    entity_description: LynnaiSensorDescription

    def __init__(self, hass: HomeAssistant, hub: LynnaiHub, entry_id: str, description: LynnaiSensorDescription) -> None:
        super().__init__(hass, hub, entry_id, description.plugin, description.bit, description.key)
        self.entity_description = description

    @property
    def native_value(self) -> float | None:
        return self.entity_description.value(self.hub)
