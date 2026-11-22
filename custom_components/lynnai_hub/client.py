"""TCP client for the hub's LAN channel (no Home Assistant imports here).

    connect to <host>:8484, send  "hello <token>"
    hub answers                    "ok <hub id>" | "denied" | "busy"
    then both ways                 "<cmd hex2> <payload hex>", and the hub sends "ping" every 30 s

A new token is only accepted for three minutes after a short press on the hub's button (pairing).
The hub keeps at most four tokens and serves two clients at a time.
"""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable
import logging
import secrets

from .const import SSDP_ST
from .protocol import CMD_REQUEST, encode_line, parse_line

_LOGGER = logging.getLogger(__name__)

CONNECT_TIMEOUT = 5
HELLO_TIMEOUT = 10
READ_TIMEOUT = 90           # the hub pings every 30 s
BACKOFF_FIRST = 5
BACKOFF_MAX = 60
BACKOFF_REJECTED = 30


class HubError(Exception):
    """Could not talk to the hub."""


class HubDenied(HubError):
    """The hub does not know our token (pairing needed)."""


class HubBusy(HubError):
    """Two clients are already connected."""


class HubWrongId(HubError):
    """Something answered, but it is not the hub we were set up with."""


def new_token() -> str:
    return secrets.token_hex(16)


async def _open(host: str, port: int, token: str) -> tuple[asyncio.StreamReader, asyncio.StreamWriter, str]:
    """Connect and say hello. Returns the streams and the hub id."""
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), CONNECT_TIMEOUT)
    except (OSError, asyncio.TimeoutError) as err:
        raise HubError(f"cannot connect to {host}:{port}: {err}") from err
    try:
        writer.write(f"hello {token}\n".encode())
        await writer.drain()
        first = (await asyncio.wait_for(reader.readline(), HELLO_TIMEOUT)).decode(errors="replace").strip()
    except (OSError, asyncio.TimeoutError) as err:
        writer.close()
        raise HubError(f"no answer from {host}:{port}: {err}") from err
    if first.startswith("ok ") and first[3:].isalnum():
        return reader, writer, first[3:]
    writer.close()
    if first == "denied":
        raise HubDenied("the hub rejected the token")
    if first == "busy":
        raise HubBusy("the hub already serves two clients")
    raise HubError(f"unexpected answer from {host}:{port}: {first!r}")


async def try_pair(host: str, port: int, token: str) -> str:
    """One connection attempt for the config flow. Returns the hub id or raises."""
    _reader, writer, hub_id = await _open(host, port, token)
    writer.close()
    try:
        await writer.wait_closed()
    except OSError:
        pass
    return hub_id


class _SsdpProtocol(asyncio.DatagramProtocol):
    def __init__(self) -> None:
        self.found: dict[str, tuple[str, int]] = {}

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        headers = {}
        for line in data.decode(errors="replace").split("\r\n")[1:]:
            key, sep, value = line.partition(":")
            if sep:
                headers[key.strip().lower()] = value.strip()
        hub_id, port = headers.get("lynnai-id"), headers.get("lynnai-port", "")
        if hub_id and hub_id.isalnum() and port.isdigit():
            self.found[hub_id] = (addr[0], int(port))


async def discover(wait: float = 3.0) -> dict[str, tuple[str, int]]:
    """Find hubs on the LAN with SSDP. Returns {hub id: (host, port)}."""
    loop = asyncio.get_running_loop()
    try:
        transport, proto = await loop.create_datagram_endpoint(_SsdpProtocol, local_addr=("0.0.0.0", 0))
    except OSError as err:
        _LOGGER.debug("SSDP search not possible: %s", err)
        return {}
    msg = "\r\n".join(
        ["M-SEARCH * HTTP/1.1", "HOST: 239.255.255.250:1900", 'MAN: "ssdp:discover"', "MX: 2", f"ST: {SSDP_ST}", "", ""]
    )
    try:
        transport.sendto(msg.encode(), ("239.255.255.250", 1900))
        await asyncio.sleep(wait)
    except OSError as err:
        _LOGGER.debug("SSDP search failed: %s", err)
    finally:
        transport.close()
    return proto.found


class HubClient:
    """Keeps one connection to the hub open and hands every message to a callback."""

    def __init__(
        self,
        host: str,
        port: int,
        token: str,
        hub_id: str,
        on_message: Callable[[int, bytes], None],
        on_connection: Callable[[bool, str | None], None],
        find_host: Callable[[], Awaitable[tuple[str, int] | None]] | None = None,
    ) -> None:
        self.host = host
        self.port = port
        self._token = token
        self._hub_id = hub_id
        self._on_message = on_message
        self._on_connection = on_connection
        self._find_host = find_host
        self._writer: asyncio.StreamWriter | None = None
        self._task: asyncio.Task | None = None
        self.connected = False

    def start(self) -> None:
        if self._task is None:
            self._task = asyncio.get_running_loop().create_task(self._run(), name=f"lynnai_hub {self._hub_id}")

    async def stop(self) -> None:
        task, self._task = self._task, None
        if task:
            task.cancel()
            try:
                await task
            except asyncio.CancelledError:
                pass
        self._drop()

    def _drop(self) -> None:
        writer, self._writer = self._writer, None
        if writer:
            writer.close()
        self.connected = False

    async def send_text(self, text: str) -> None:
        """Send one request, e.g. 'f06@a1b2c3 b1=1'."""
        if not self._writer:
            raise HubError("not connected to the hub")
        _LOGGER.debug("-> hub: %s", text)
        try:
            self._writer.write(encode_line(CMD_REQUEST, text.encode()))
            await self._writer.drain()
        except OSError as err:
            raise HubError(f"sending to the hub failed: {err}") from err

    async def _run(self) -> None:
        backoff = BACKOFF_FIRST
        while True:
            reason: str | None = None
            try:
                reader, writer, hub_id = await _open(self.host, self.port, self._token)
                if hub_id != self._hub_id:
                    writer.close()
                    raise HubWrongId(f"expected hub {self._hub_id}, {self.host} is {hub_id}")
                self._writer = writer
                self.connected = True
                backoff = BACKOFF_FIRST
                _LOGGER.info("Connected to hub %s at %s:%s", hub_id, self.host, self.port)
                self._on_connection(True, None)
                await self._read(reader)
                reason = "connection lost"
            except HubDenied:
                reason, backoff = "pairing needed", BACKOFF_REJECTED
            except HubBusy:
                reason, backoff = "hub busy", BACKOFF_REJECTED
            except HubWrongId as err:
                reason = str(err)
                await self._relocate()
            except HubError as err:
                reason = str(err)
                await self._relocate()
            finally:
                was = self.connected
                self._drop()
            if was or reason:
                _LOGGER.debug("Hub %s: %s; retry in %s s", self._hub_id, reason, backoff)
            self._on_connection(False, reason)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, BACKOFF_MAX)

    async def _relocate(self) -> None:
        """The address may have changed (DHCP): look the hub up again."""
        if not self._find_host:
            return
        try:
            found = await self._find_host()
        except Exception:  # noqa: BLE001 - discovery is best effort
            _LOGGER.debug("Looking up hub %s failed", self._hub_id, exc_info=True)
            return
        if found and found != (self.host, self.port):
            _LOGGER.info("Hub %s moved to %s:%s", self._hub_id, *found)
            self.host, self.port = found

    async def _read(self, reader: asyncio.StreamReader) -> None:
        while True:
            try:
                raw = await asyncio.wait_for(reader.readline(), READ_TIMEOUT)
            except (OSError, asyncio.TimeoutError, ValueError):
                return
            if not raw:
                return
            msg = parse_line(raw.decode(errors="replace"))
            if msg is None:         # "ping" and anything we do not understand
                continue
            try:
                self._on_message(*msg)
            except Exception:  # noqa: BLE001 - one bad message must not end the connection
                _LOGGER.exception("Handling hub message %02x failed", msg[0])
