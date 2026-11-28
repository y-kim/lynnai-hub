"""Sensors wired to the hub (room temperature/humidity, gas meter), the boiler's gas bill and the hub's own status."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass

from homeassistant.components.sensor import SensorDeviceClass, SensorEntity, SensorEntityDescription, SensorStateClass
from homeassistant.const import PERCENTAGE, EntityCategory, UnitOfTemperature, UnitOfVolume, UnitOfVolumeFlowRate
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddEntitiesCallback
from homeassistant.util import dt as dt_util

from . import LynnaiConfigEntry, protocol as p
from .entity import BoilerEntity, HubEntity, HubModeEntity, PluginEntity, setup_dynamic
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
    # The meter total feeds Home Assistant's energy dashboard (gas consumption). 'total' with last_reset (not
    # total_increasing): setting the meter reading on the hub jumps the number without gas flowing, and that jump must
    # not be counted as use — the hub object notes the moment (gas_reset) and the sensor reports it as last_reset.
    LynnaiSensorDescription(
        key="gas_total", plugin="gas", bit=p.PLUGIN_GAS,
        device_class=SensorDeviceClass.GAS, native_unit_of_measurement=UnitOfVolume.CUBIC_METERS,
        state_class=SensorStateClass.TOTAL, suggested_display_precision=2,
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
        if hub.mode is not None:
            out.append(HubStatus(hass, hub, entry.entry_id))
        if hub.chip_temp is not None:
            out.append(HubTemperature(hub, entry.entry_id))
        if hub.bill is not None:
            for mac in hub.boilers:
                out += [BillUsage(hass, hub, entry.entry_id, mac), BillCost(hass, hub, entry.entry_id, mac)]
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

    @property
    def last_reset(self):
        return self.hub.gas_reset if self.entity_description.key == "gas_total" else None


class _Bill(BoilerEntity, SensorEntity):
    """This billing period's gas and its estimated bill, on the boiler device (SmartThings shows them on the boiler card).
    The hub counts them with what its admin page picks: the gas meter, or the boiler's own totals (whole m3, hourly)."""

    _attr_state_class = SensorStateClass.TOTAL       # starts again from 0 each billing period

    @property
    def available(self) -> bool:
        return super().available and self.hub.connected and self.hub.bill is not None

    @property
    def extra_state_attributes(self) -> dict[str, str] | None:
        return {"basis": self.hub.bill.basis} if self.hub.bill else None

    @property
    def last_reset(self):
        return self.hub.bill_reset          # a new billing period started (the values went back down)


class BillUsage(_Bill):
    _attr_device_class = SensorDeviceClass.GAS
    _attr_native_unit_of_measurement = UnitOfVolume.CUBIC_METERS

    def __init__(self, hass: HomeAssistant, hub: LynnaiHub, entry_id: str, mac: str) -> None:
        super().__init__(hass, hub, entry_id, mac, "gas_period")

    @property
    def native_value(self) -> float | None:
        return self.hub.bill.m3 if self.hub.bill else None


class BillCost(_Bill):
    _attr_device_class = SensorDeviceClass.MONETARY
    _attr_native_unit_of_measurement = "KRW"
    _attr_suggested_display_precision = 0

    def __init__(self, hass: HomeAssistant, hub: LynnaiHub, entry_id: str, mac: str) -> None:
        super().__init__(hass, hub, entry_id, mac, "gas_period_cost")

    @property
    def native_value(self) -> int | None:
        return self.hub.bill.won if self.hub.bill else None


# The status is a sentence with a time in it, which Home Assistant cannot translate; pick by the configured language.
# The reason is what the hub found when it tried (protocol.FALLBACK_REASONS); None = an older hub that does not say.
_STATUS = {
    "ko": {"proxy": "프록시 모드", "standalone": "단독 모드",
           "fallback": "단독 모드 ({since}부터 {reason})", "fallback_unknown": "단독 모드 ({reason})",
           "reason": {None: "린나이 서버 접속 불가", "wifi": "집 Wi-Fi 끊김", "connect": "린나이 서버 연결 안 됨",
                      "tls": "린나이 서버 보안 연결 실패", "refused": "린나이 서버 로그인 거절(코드 {code})",
                      "noreply": "린나이 서버 로그인 응답 없음", "closed": "린나이 서버가 연결을 끊음",
                      "dns": "린나이 서버 주소 조회 실패"}},
    "en": {"proxy": "Proxy", "standalone": "Standalone",
           "fallback": "Standalone ({reason} since {since})", "fallback_unknown": "Standalone ({reason})",
           "reason": {None: "Rinnai server unreachable", "wifi": "home Wi-Fi down",
                      "connect": "cannot connect to the Rinnai server",
                      "tls": "secure connection to the Rinnai server fails",
                      "refused": "Rinnai server refuses the login (code {code})",
                      "noreply": "Rinnai server does not answer the login",
                      "closed": "Rinnai server closes the connection",
                      "dns": "cannot look up the Rinnai server's address"}},
}


class HubStatus(HubModeEntity, SensorEntity):
    """What the hub is doing right now. The mode select is what it is set to; this also shows when it had to
    fall back to serving the devices itself because the Rinnai server cannot be reached, and since when."""

    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, hass: HomeAssistant, hub: LynnaiHub, entry_id: str) -> None:
        super().__init__(hub, entry_id, "hub_status")
        self._text = _STATUS["ko"] if (hass.config.language or "").startswith("ko") else _STATUS["en"]

    @property
    def native_value(self) -> str | None:
        mode = self.hub.mode
        if mode is None:
            return None
        if mode.status != "fallback":
            return self._text[mode.status]
        reasons = self._text["reason"]
        reason = reasons.get(mode.reason, reasons[None]).format(code=mode.code)
        if mode.since is None:
            return self._text["fallback_unknown"].format(reason=reason)
        since = dt_util.as_local(dt_util.utc_from_timestamp(mode.since))
        same_day = since.date() == dt_util.now().date()
        return self._text["fallback"].format(since=since.strftime("%H:%M" if same_day else "%m/%d %H:%M"), reason=reason)

    @property
    def extra_state_attributes(self) -> dict[str, str | int] | None:
        mode = self.hub.mode
        if mode is None or mode.status != "fallback":
            return None
        attrs: dict[str, str | int] = {}
        if mode.since is not None:
            attrs["unreachable_since"] = dt_util.utc_from_timestamp(mode.since).isoformat()
        if mode.reason is not None:
            attrs["reason"] = mode.reason
            if mode.reason == "refused":
                attrs["refusal_code"] = mode.code
        return attrs or None


class HubTemperature(HubEntity, SensorEntity):
    """The hub's own temperature (its chip, not the room). The hub sends it only while "허브 내부 온도" is ticked
    for the LAN channel on its admin page, so the entity appears once a value has arrived."""

    _attr_device_class = SensorDeviceClass.TEMPERATURE
    _attr_native_unit_of_measurement = UnitOfTemperature.CELSIUS
    _attr_state_class = SensorStateClass.MEASUREMENT
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_suggested_display_precision = 1

    def __init__(self, hub: LynnaiHub, entry_id: str) -> None:
        super().__init__(hub, entry_id, "hub_temperature")

    @property
    def available(self) -> bool:
        return self.hub.connected            # not the last value from before a disconnect

    @property
    def native_value(self) -> float | None:
        return self.hub.chip_temp
