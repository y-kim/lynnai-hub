"""Setting the hub up: address, then pairing with a button press."""

from __future__ import annotations

from unittest.mock import patch

import pytest

from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType

from custom_components.lynnai_hub.const import DOMAIN

from .conftest import HUB_ID


@pytest.fixture(autouse=True)
def _sockets(socket_enabled):
    yield


async def test_pairing_needs_the_button(hass: HomeAssistant, fake_hub):
    with patch("custom_components.lynnai_hub.config_flow.discover", return_value={HUB_ID: ("127.0.0.1", fake_hub.port)}):
        result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": "user"})
    assert result["type"] is FlowResultType.FORM and result["step_id"] == "user"
    assert HUB_ID in result["description_placeholders"]["found"]

    result = await hass.config_entries.flow.async_configure(result["flow_id"], {"host": "127.0.0.1", "port": fake_hub.port})
    assert result["step_id"] == "pair" and not result.get("errors")

    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})      # button not pressed yet
    assert result["type"] is FlowResultType.FORM and result["errors"] == {"base": "denied"}

    fake_hub.pairing_open = True                                                        # button pressed
    with patch("custom_components.lynnai_hub.async_setup_entry", return_value=True):
        result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"]["hub_id"] == HUB_ID and result["data"]["token"] in fake_hub.tokens
    assert result["result"].unique_id == HUB_ID
    assert result["data"]["v2"] is True                 # paired with v1, then the hub proved itself with v2


async def test_unreachable_host(hass: HomeAssistant):
    with patch("custom_components.lynnai_hub.config_flow.discover", return_value={}):
        result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": "user"})
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {"host": "127.0.0.1", "port": 1})
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["errors"] == {"base": "cannot_connect"}


async def test_discovered_by_ssdp(hass: HomeAssistant, fake_hub):
    """The hub answers SSDP searches, so Home Assistant lists it under discovered integrations."""
    try:
        from homeassistant.helpers.service_info.ssdp import SsdpServiceInfo
    except ImportError:  # Home Assistant before 2025.2
        from homeassistant.components.ssdp import SsdpServiceInfo

    info = SsdpServiceInfo(
        ssdp_usn=f"uuid:lynnai-{HUB_ID}::urn:lynnai-com:device:hub:1",
        ssdp_st="urn:lynnai-com:device:hub:1",
        ssdp_location="http://127.0.0.1/",
        upnp={},
        ssdp_headers={"LYNNAI-ID": HUB_ID, "LYNNAI-PORT": str(fake_hub.port), "location": "http://127.0.0.1/"},
    )
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": "ssdp"}, data=info)
    assert result["type"] is FlowResultType.FORM and result["step_id"] == "pair"
    flows = hass.config_entries.flow.async_progress_by_handler(DOMAIN)
    assert flows and flows[0]["context"]["title_placeholders"] == {"hub_id": HUB_ID}      # shown as "LYNNAI Hub 8TEST1"

    fake_hub.pairing_open = True
    with patch("custom_components.lynnai_hub.async_setup_entry", return_value=True):
        result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"]["host"] == "127.0.0.1" and result["data"]["port"] == fake_hub.port

    # found again later (e.g. after an address change): no second entry, the address is updated
    info2 = SsdpServiceInfo(
        ssdp_usn=info.ssdp_usn, ssdp_st=info.ssdp_st, ssdp_location="http://127.0.0.2/", upnp={},
        ssdp_headers={"LYNNAI-ID": HUB_ID, "LYNNAI-PORT": str(fake_hub.port)},
    )
    with patch("custom_components.lynnai_hub.async_setup_entry", return_value=True):
        result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": "ssdp"}, data=info2)
        await hass.async_block_till_done()
    assert result["type"] is FlowResultType.ABORT and result["reason"] == "already_configured"
    # an announcement is not proof: the address is not moved by it (the running client re-finds a moved hub itself)
    assert hass.config_entries.async_entries(DOMAIN)[0].data["host"] == "127.0.0.1"


async def test_pairing_with_older_firmware(hass: HomeAssistant, fake_hub):
    """Firmware without v2 closes on 'hello2': pairing falls back to v1 and the entry is not marked v2."""
    fake_hub.legacy = True
    fake_hub.pairing_open = True
    with patch("custom_components.lynnai_hub.config_flow.discover", return_value={}):
        result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": "user"})
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {"host": "127.0.0.1", "port": fake_hub.port})
    with patch("custom_components.lynnai_hub.async_setup_entry", return_value=True):
        result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["type"] is FlowResultType.CREATE_ENTRY and result["data"]["v2"] is False
