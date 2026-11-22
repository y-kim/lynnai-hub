"""LYNNAI Hub: Rinnai boiler controllers and room thermostats through a local LYNNAI hub."""

from __future__ import annotations

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import CONF_HOST, CONF_PORT, Platform
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.dispatcher import async_dispatcher_send

from .client import HubClient, discover
from .const import CONF_HUB_ID, CONF_TOKEN, DEFAULT_PORT, DOMAIN, MANUFACTURER, SIGNAL_NEW, SIGNAL_UPDATE
from .hub import LynnaiHub

PLATFORMS = [
    Platform.BINARY_SENSOR,
    Platform.CLIMATE,
    Platform.NUMBER,
    Platform.SELECT,
    Platform.SENSOR,
    Platform.SWITCH,
    Platform.WATER_HEATER,
]

type LynnaiConfigEntry = ConfigEntry[LynnaiHub]


async def async_setup_entry(hass: HomeAssistant, entry: LynnaiConfigEntry) -> bool:
    hub = LynnaiHub(entry.data[CONF_HUB_ID])

    async def find_host() -> tuple[str, int] | None:
        return (await discover()).get(hub.hub_id)

    hub.client = HubClient(
        entry.data[CONF_HOST],
        entry.data.get(CONF_PORT, DEFAULT_PORT),
        entry.data[CONF_TOKEN],
        hub.hub_id,
        hub.handle_message,
        hub.handle_connection,
        find_host,
    )

    @callback
    def on_update() -> None:
        async_dispatcher_send(hass, SIGNAL_UPDATE.format(entry.entry_id))

    @callback
    def on_new() -> None:
        async_dispatcher_send(hass, SIGNAL_NEW.format(entry.entry_id))

    hub.set_callbacks(on_update, on_new)
    entry.runtime_data = hub

    dr.async_get(hass).async_get_or_create(
        config_entry_id=entry.entry_id,
        identifiers={(DOMAIN, hub.hub_id)},
        manufacturer=MANUFACTURER,
        model="LYNNAI Hub",
        name=f"LYNNAI Hub {hub.hub_id}",
        configuration_url=f"http://{entry.data[CONF_HOST]}/admin",
    )

    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    # Devices show up when the hub reports them, so entities are added after the connection is up.
    hub.client.start()
    return True


async def async_unload_entry(hass: HomeAssistant, entry: LynnaiConfigEntry) -> bool:
    unloaded = await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
    if unloaded and entry.runtime_data.client:
        await entry.runtime_data.client.stop()
    return unloaded


async def async_remove_config_entry_device(
    hass: HomeAssistant, entry: LynnaiConfigEntry, device: dr.DeviceEntry
) -> bool:
    """Allow removing a device by hand (one that was deleted on the hub stays listed otherwise)."""
    return (DOMAIN, entry.runtime_data.hub_id) not in device.identifiers
