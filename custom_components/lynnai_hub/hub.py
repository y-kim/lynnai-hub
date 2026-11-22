"""State of one LYNNAI hub and the commands that change it (no Home Assistant imports here)."""

from __future__ import annotations

from collections.abc import Callable
import logging

from . import protocol as p
from .client import HubClient

_LOGGER = logging.getLogger(__name__)

STANDALONE = "000000"       # a thermostat the user marked as having no boiler on the hub


class LynnaiHub:
    """Decoded state of everything behind one hub, kept current from the hub's messages."""

    def __init__(self, hub_id: str) -> None:
        self.hub_id = hub_id
        self.client: HubClient | None = None
        self.connected = False
        self.problem: str | None = None
        self.boilers: dict[str, p.BoilerState] = {}
        self.zones: dict[str, dict[str, p.ZoneState]] = {}      # thermostat mac -> zone id -> state
        self.pairs: dict[str, str] = {}
        self.links: set[tuple[int, str]] | None = None          # None until the hub told us
        self.room: p.RoomSensorState | None = None
        self.gas: p.GasState | None = None
        self.plugins = p.PluginState()
        self._on_update: Callable[[], None] = lambda: None
        self._on_new: Callable[[], None] = lambda: None

    def set_callbacks(self, on_update: Callable[[], None], on_new: Callable[[], None]) -> None:
        self._on_update, self._on_new = on_update, on_new

    # ---- messages from the hub ----

    def handle_connection(self, connected: bool, problem: str | None) -> None:
        changed = connected != self.connected or problem != self.problem
        self.connected, self.problem = connected, problem
        if not connected:
            self.links = None       # availability comes back with the next links message
        if changed:
            self._on_update()

    def handle_message(self, cmd: int, payload: bytes) -> None:
        new = False
        if cmd == p.CMD_STATE and len(payload) >= 4:
            kind, mac, blob = payload[0], payload[1:4].hex(), payload[4:]
            if kind == p.KIND_BOILER:
                state = p.decode_boiler(blob)
                if state is None:
                    return
                new = mac not in self.boilers
                self.boilers[mac] = state
            elif kind == p.KIND_THERMOSTAT:
                zones = p.decode_zones(blob)
                known = self.zones.get(mac, {})
                new = any(z not in known for z in zones)
                # keep a room that is briefly missing from a report (the SmartThings driver waits three reports)
                self.zones[mac] = {**{z: s for z, s in known.items() if z not in zones}, **zones}
            else:
                return
        elif cmd == p.CMD_LINKS:
            self.links = p.decode_links(payload)
        elif cmd == p.CMD_ROOM:
            room = p.decode_room(payload)
            if room is None:
                return
            # a humidity reading that appears later (sensor swapped) needs its entity too
            new = self.room is None or (self.room.humidity is None and room.humidity is not None)
            self.room = room
        elif cmd == p.CMD_GAS:
            new = self.gas is None
            self.gas = p.decode_gas(payload) or self.gas
        elif cmd == p.CMD_PLUGINS:
            self.plugins = p.decode_plugins(payload)
        elif cmd == p.CMD_PAIRS:
            self.pairs = p.decode_pairs(payload)
        elif cmd == p.CMD_FORGET and len(payload) >= 4:
            kind, mac = payload[0], payload[1:4].hex()
            if kind == p.KIND_BOILER:
                self.boilers.pop(mac, None)
            elif kind == p.KIND_THERMOSTAT:
                self.zones.pop(mac, None)
        else:
            return                  # alive, expose, tariff: nothing to show
        if new:
            self._on_new()
        self._on_update()

    # ---- what is reachable ----

    def device_online(self, kind: int, mac: str) -> bool:
        if not self.connected:
            return False
        return self.links is None or (kind, mac) in self.links

    def boiler_of(self, thermostat_mac: str) -> str | None:
        """The boiler a thermostat belongs to: the one set on the hub, else the only boiler there is."""
        paired = self.pairs.get(thermostat_mac)
        if paired:
            return paired if paired in self.boilers else None
        return next(iter(self.boilers)) if len(self.boilers) == 1 else None

    def zones_of_boiler(self, boiler_mac: str) -> list[tuple[str, str]]:
        return [
            (tmac, zone_id)
            for tmac, zones in self.zones.items()
            if self.boiler_of(tmac) == boiler_mac and self.device_online(p.KIND_THERMOSTAT, tmac)
            for zone_id in zones
        ]

    def boiler_away(self, boiler_mac: str) -> bool:
        """A boiler counts as 'away' when every room attached to it is away."""
        rooms = self.zones_of_boiler(boiler_mac)
        return bool(rooms) and all(self.zones[t][z].away for t, z in rooms)

    # ---- commands ----

    async def _send(self, requests: list[str]) -> None:
        if not self.client:
            raise RuntimeError("hub client not started")
        for text in requests:
            await self.client.send_text(text)

    async def refresh(self) -> None:
        await self._send(["refresh"])

    async def set_boiler_switch(self, mac: str, name: str, on: bool) -> None:
        _pos, code = p.BOILER_SWITCHES[name]
        await self._send([p.boiler_cmd(mac, code, p.onoff(on))])

    async def set_boiler_heating(self, mac: str, on: bool) -> None:
        """Heating on also takes the attached rooms out of away mode (as the vendor's app shows it)."""
        requests = []
        if on:
            requests += self._away_requests(mac, False)
        requests.append(p.boiler_cmd(mac, "b1", p.onoff(on)))
        await self._send(requests)

    async def set_boiler_away(self, mac: str, on: bool) -> None:
        """Away is a per-room setting on the thermostats; the boiler's own away code does nothing."""
        await self._send(self._away_requests(mac, on))

    def _away_requests(self, boiler_mac: str, on: bool) -> list[str]:
        return [
            p.zone_cmd(tmac, zone_id, f"b3={p.onoff(on)}")
            for tmac, zone_id in self.zones_of_boiler(boiler_mac)
            if self.zones[tmac][zone_id].away != on
        ]

    async def set_boiler_heat_mode(self, mac: str, mode: str) -> None:
        await self._send([p.boiler_cmd(mac, "b4", "1" if mode == p.HEAT_MODE_ROOM else "0")])

    async def set_boiler_heat_setpoint(self, mac: str, value: float) -> None:
        state = self.boilers[mac]
        if state.heat_mode == p.HEAT_MODE_ROOM:
            v = p.snap(value, p.ROOM_TEMPS, state.room_setpoint)
            await self._send([p.boiler_cmd(mac, "d4", f"{v:02X}")])
        else:
            v = p.snap(value, p.ONDOL_TEMPS, state.ondol_setpoint)
            await self._send([p.boiler_cmd(mac, "d2", f"{v:02X}")])

    async def set_hot_water_setpoint(self, mac: str, value: float) -> None:
        v = p.snap(value, p.HOT_WATER_TEMPS, self.boilers[mac].hot_water_setpoint)
        await self._send([p.boiler_cmd(mac, "d1", f"{v:02X},df=0")])

    async def set_zone_power(self, tmac: str, zone_id: str, on: bool) -> None:
        """Turning a room on means 'heat this room': wake its boiler and leave away mode first."""
        requests = []
        if on:
            bmac = self.boiler_of(tmac)
            boiler = self.boilers.get(bmac) if bmac else None
            if boiler and not boiler.power:
                requests.append(p.boiler_cmd(bmac, "80", "1"))
            if boiler and not boiler.heat:
                requests.append(p.boiler_cmd(bmac, "b1", "1"))
            zone = self.zones.get(tmac, {}).get(zone_id)
            if zone and zone.away:
                requests.append(p.zone_cmd(tmac, zone_id, "b3=0"))
        requests.append(p.zone_cmd(tmac, zone_id, f"b1={p.onoff(on)}"))
        await self._send(requests)

    async def set_zone_target(self, tmac: str, zone_id: str, value: float) -> None:
        v = max(p.ZONE_MIN_TEMP, min(p.ZONE_MAX_TEMP, round(value)))
        await self._send([p.zone_cmd(tmac, zone_id, f"d4={v:02X}")])

    async def set_zone_away(self, tmac: str, zone_id: str, on: bool) -> None:
        await self._send([p.zone_cmd(tmac, zone_id, f"b3={p.onoff(on)}")])

    async def set_zone_reservation(self, tmac: str, zone_id: str, hours: int | None) -> None:
        """hours = 1..24 starts a reservation of that length; None cancels it."""
        if hours is None:
            await self._send([p.zone_cmd(tmac, zone_id, "96=0")])
        else:
            h = max(1, min(24, int(hours)))
            await self._send([p.zone_cmd(tmac, zone_id, f"96=1,97={h:02x}")])
