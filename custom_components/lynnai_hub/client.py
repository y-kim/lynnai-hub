"""TCP client for the hub's LAN channel (no Home Assistant imports here).

    v2 (the token never goes over the wire, firmware v0.10+):
      -> "hello2 <cn>"          cn = 32 hex random
      <- "chal <sn>"            sn = the hub's random
      -> "auth <HMAC-SHA256(token, 'c|cn|sn')>"
      <- "ok <hub id> <HMAC-SHA256(token, 's|sn|cn|hub id')>" | "denied" | "busy"
    v1 (pairing a new token, older firmware):
      -> "hello <token>"        <- "ok <hub id>" | "denied" | "busy"
    then both ways              "<cmd hex2> <payload hex>", and the hub sends "ping" every 30 s

Once a hub has answered v2, we never send it the token in plain text again (something on the LAN that pretends to be
the hub — answering "denied" or acting like old firmware — must not be able to collect the token).

A new token is only accepted for three minutes after a short press on the hub's button (pairing).
The hub keeps at most four tokens and serves two clients at a time.
"""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable
import hashlib
import hmac
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


class HubLegacy(HubError):
    """The hub does not speak v2 (older firmware): it closed without a challenge."""


def new_token() -> str:
    return secrets.token_hex(16)


def proof(token: str, *parts: str) -> str:
    """HMAC-SHA256(token, 'a|b|…') in hex — the same as hub_lan.c / hub_lan.py."""
    return hmac.new(token.encode(), "|".join(parts).encode(), hashlib.sha256).hexdigest()


async def _line(reader: asyncio.StreamReader) -> str:
    return (await asyncio.wait_for(reader.readline(), HELLO_TIMEOUT)).decode(errors="replace").strip()


async def _open(
    host: str, port: int, token: str, v1: bool = False
) -> tuple[asyncio.StreamReader, asyncio.StreamWriter, str, bool]:
    """Connect and authenticate. Returns the streams, the hub id and whether it was v2 (the hub proved it knows the
    token). v1=True sends the token itself (pairing, older firmware) — callers only do that before any v2 success."""
    try:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), CONNECT_TIMEOUT)
    except (OSError, asyncio.TimeoutError) as err:
        raise HubError(f"cannot connect to {host}:{port}: {err}") from err
    try:
        if v1:
            writer.write(f"hello {token}\n".encode())
            await writer.drain()
            first = await _line(reader)
            if first.startswith("ok ") and first[3:].isalnum():
                return reader, writer, first[3:], False
        else:
            cn = secrets.token_hex(16)
            writer.write(f"hello2 {cn}\n".encode())
            await writer.drain()
            chal = await _line(reader)
            sn = chal[5:].lower() if chal.startswith("chal ") else ""
            if len(sn) != 32 or any(c not in "0123456789abcdef" for c in sn):
                if not chal:
                    raise HubLegacy("the hub closed without a challenge (older firmware)")
                first = chal
            else:
                writer.write(f"auth {proof(token, 'c', cn, sn)}\n".encode())
                await writer.drain()
                first = await _line(reader)
                parts = first.split(" ")
                if len(parts) == 3 and parts[0] == "ok" and parts[1].isalnum():
                    if hmac.compare_digest(parts[2], proof(token, "s", sn, cn, parts[1])):
                        return reader, writer, parts[1], True
                    raise HubWrongId(f"{host}:{port} does not know our token (its proof does not match)")
    except (OSError, asyncio.TimeoutError, ValueError) as err:
        writer.close()
        raise HubError(f"no answer from {host}:{port}: {err}") from err
    except HubError:
        writer.close()
        raise
    writer.close()
    if first == "denied":
        raise HubDenied("the hub rejected the token")
    if first == "busy":
        raise HubBusy("the hub already serves two clients")
    raise HubError(f"unexpected answer from {host}:{port}: {first!r}")


async def _close(writer: asyncio.StreamWriter) -> None:
    writer.close()
    try:
        await writer.wait_closed()
    except OSError:
        pass


async def try_pair(host: str, port: int, token: str) -> tuple[str, bool]:
    """Connection attempts for the config flow (a new token, so the hub cannot know it yet).
    Returns (hub id, v2): v2 = the hub proved it knows the token, so we never send it in plain text again."""
    try:
        _reader, writer, hub_id, v2 = await _open(host, port, token)
    except (HubDenied, HubLegacy):
        _reader, writer, hub_id, v2 = await _open(host, port, token, v1=True)   # registers it (pairing window)
        await _close(writer)
        try:                                                                    # newer firmware: confirm v2 now
            _reader, writer, hub_id2, v2 = await _open(host, port, token)
            if hub_id2 != hub_id:
                raise HubWrongId(f"expected hub {hub_id}, got {hub_id2}")
        except (HubDenied, HubLegacy):
            return hub_id, False
    await _close(writer)
    return hub_id, v2


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
        if hub_id and hub_id.isalnum() and len(hub_id) <= 32 and port.isdigit() and 1 <= int(port) <= 65535:
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
        v2_seen: bool = False,
        on_verified: Callable[[str, int], None] | None = None,
    ) -> None:
        self.host = host
        self.port = port
        self._token = token
        self._hub_id = hub_id
        self._on_message = on_message
        self._on_connection = on_connection
        self._find_host = find_host
        self.v2_seen = v2_seen              # the hub has proved itself once: never send the token in plain text
        self._on_verified = on_verified     # called after a v2 connection to a new address (persist it)
        self._try_v1 = False
        self._verified_at: tuple[str, int] | None = None
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
                use_v1, self._try_v1 = self._try_v1 and not self.v2_seen, False
                reader, writer, hub_id, v2 = await _open(self.host, self.port, self._token, v1=use_v1)
                if hub_id != self._hub_id:
                    writer.close()
                    raise HubWrongId(f"expected hub {self._hub_id}, {self.host} is {hub_id}")
                if v2 and (not self.v2_seen or self._verified_at != (self.host, self.port)):
                    self.v2_seen = True
                    self._verified_at = (self.host, self.port)
                    if self._on_verified:
                        self._on_verified(self.host, self.port)
                self._writer = writer
                self.connected = True
                backoff = BACKOFF_FIRST
                _LOGGER.info("Connected to hub %s at %s:%s", hub_id, self.host, self.port)
                self._on_connection(True, None)
                await self._read(reader)
                reason = "connection lost"
            except HubLegacy as err:
                reason = str(err)
                self._try_v1 = not self.v2_seen         # older firmware: v1 next time (never after a v2 success)
            except HubDenied:
                reason, backoff = "pairing needed", BACKOFF_REJECTED
                self._try_v1 = not self.v2_seen         # the hub does not know the token yet: v1 registers it
            except HubBusy:
                reason, backoff = "hub busy", BACKOFF_REJECTED
            except HubWrongId as err:
                reason = str(err)
                await self._relocate()
            except HubError as err:
                reason = str(err)
                await self._relocate()
            except asyncio.CancelledError:
                raise
            except Exception as err:  # noqa: BLE001 - nothing may end the reconnect loop for good
                _LOGGER.exception("Unexpected error talking to hub %s", self._hub_id)
                reason = f"unexpected error: {err}"
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
            _LOGGER.debug("<- hub: %02x %s", msg[0], msg[1].hex())
            try:
                self._on_message(*msg)
            except Exception:  # noqa: BLE001 - one bad message must not end the connection
                _LOGGER.exception("Handling hub message %02x failed", msg[0])
