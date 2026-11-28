"""Config flow: find the hub, then pair with it (short press on the hub's button)."""

from __future__ import annotations

from typing import Any
from urllib.parse import urlparse

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST, CONF_PORT

from .client import HubBusy, HubDenied, HubError, discover, new_token, try_pair
from .const import CONF_HUB_ID, CONF_TOKEN, CONF_V2, DEFAULT_PORT, DOMAIN


class LynnaiHubConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    def __init__(self) -> None:
        self._host: str | None = None
        self._port: int = DEFAULT_PORT
        self._expect_id: str | None = None
        self._token = new_token()

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """Ask for the hub's address, pre-filled when a hub answers the search."""
        if user_input is not None:
            self._host = user_input[CONF_HOST].strip()
            self._port = user_input[CONF_PORT]
            self._expect_id = None
            return await self.async_step_pair()

        found = await discover(2.5)
        configured = {e.unique_id for e in self._async_current_entries()}
        fresh = {hub_id: addr for hub_id, addr in found.items() if hub_id not in configured}
        host, port = next(iter(fresh.values()), ("", DEFAULT_PORT))
        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {vol.Required(CONF_HOST, default=host): str,
                 vol.Required(CONF_PORT, default=port): vol.All(vol.Coerce(int), vol.Range(min=1, max=65535))}
            ),
            description_placeholders={"found": ", ".join(f"{i} ({a[0]})" for i, a in fresh.items()) or "-"},
        )

    async def async_step_ssdp(self, discovery_info: Any) -> ConfigFlowResult:
        """A hub announced itself on the network."""
        headers = {str(k).lower(): v for k, v in (getattr(discovery_info, "ssdp_headers", None) or {}).items()}
        hub_id = headers.get("lynnai-id")
        if not hub_id:                      # fall back to the USN: uuid:lynnai-<id>::urn:…
            usn = getattr(discovery_info, "ssdp_usn", "") or ""
            if usn.startswith("uuid:lynnai-"):
                hub_id = usn[len("uuid:lynnai-") :].split("::")[0]
        host = urlparse(getattr(discovery_info, "ssdp_location", "") or "").hostname
        if not hub_id or not host:
            return self.async_abort(reason="not_lynnai_hub")
        try:
            port = int(headers.get("lynnai-port", DEFAULT_PORT))
        except (TypeError, ValueError):
            port = DEFAULT_PORT

        await self.async_set_unique_id(hub_id)
        # An announcement is not proof: do not move a configured hub to the announced address (anything on the LAN can
        # announce our hub id). The running client finds a moved hub itself and keeps the address only after the hub
        # has proved it knows the token.
        self._abort_if_unique_id_configured()
        self._host, self._port, self._expect_id = host, port, hub_id
        self.context["title_placeholders"] = {"hub_id": hub_id}
        return await self.async_step_pair()

    async def async_step_pair(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        """The hub only takes a new token for three minutes after a short button press."""
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                hub_id, v2 = await try_pair(self._host, self._port, self._token)
            except HubDenied:
                errors["base"] = "denied"
            except HubBusy:
                errors["base"] = "busy"
            except HubError:
                errors["base"] = "cannot_connect"
            else:
                if self._expect_id and hub_id != self._expect_id:
                    errors["base"] = "wrong_hub"
                else:
                    await self.async_set_unique_id(hub_id)
                    self._abort_if_unique_id_configured()
                    return self.async_create_entry(
                        title=f"LYNNAI Hub {hub_id}",
                        data={CONF_HOST: self._host, CONF_PORT: self._port, CONF_TOKEN: self._token, CONF_HUB_ID: hub_id,
                              CONF_V2: v2},
                    )
        return self.async_show_form(
            step_id="pair",
            data_schema=vol.Schema({}),
            errors=errors,
            description_placeholders={"host": self._host or ""},
        )
