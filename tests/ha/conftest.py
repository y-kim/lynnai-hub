"""Fixtures: a fake LYNNAI hub that speaks the LAN channel, and custom integrations enabled."""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import secrets

import pytest

HUB_ID = "8TEST1"

# Captured state blobs (same ones the SmartThings driver's simulator uses).
F06_ON = "10110000003c041001800fff0000055000000000000000000000000000000000"
F06_OFF = "00010000003c041001800fff0000055000000000000000000000000000000000"
BOILER = "a1b2c3"
THERMO = "d4e5f6"


def f09(z1: int = 0x83, z2: int = 0x81, living: int = 0x83) -> str:
    """Thermostat controller blob with room 1, room 2 and the living room (0x83 on, 0x81 off)."""
    b = bytearray(54)
    for stat, st, cur, target in ((0, z1, 20, 23), (7, z2, 21, 18), (42, living, 22, 24)):
        b[stat], b[stat + 3], b[stat + 4] = st, cur, target
    return b.hex()


def _proof(token: str, *parts: str) -> str:
    return hmac.new(token.encode(), "|".join(parts).encode(), hashlib.sha256).hexdigest()


class FakeHub:
    """Speaks the LAN channel like firmware/esp32/main/hub_lan.c: v2 ('hello2' → 'chal' → 'auth' → 'ok <id> <proof>')
    and v1 ('hello <token>', pairing). legacy=True acts like firmware before v2 (closes on 'hello2').
    impostor=True knows no token: answers v2 with a wrong proof and records any token it is sent in plain text."""

    def __init__(self) -> None:
        self.legacy = False
        self.impostor = False
        self.plain_tokens: list[str] = []               # tokens received in plain text ("hello <token>")
        self.v2_logins = 0
        self.tokens: set[str] = set()
        self.pairing_open = False
        self.requests: list[str] = []
        self.writers: list[asyncio.StreamWriter] = []
        self.server: asyncio.AbstractServer | None = None
        self.port = 0
        self.on_hello: list[tuple[int, bytes]] = []     # sent to every client right after "ok"
        self.mode = [0, 0, 1, 0, 0, 0, 0]               # [mode][fallback active][auto fallback][since u32] as in HUB_RK_CMD_MODE
        self.preheat_off = 0                            # HUB_RK_CMD_BOILCFG: preheat auto-off minutes (0 = never)

    async def start(self) -> None:
        self.server = await asyncio.start_server(self._serve, "127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]

    async def stop(self) -> None:
        for w in self.writers:
            w.close()
        if self.server:
            self.server.close()
            await self.server.wait_closed()

    async def _serve(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        line = (await reader.readline()).decode().strip()
        ok = f"ok {HUB_ID}\n"
        if line.startswith("hello2 "):
            if self.legacy:                             # older firmware does not know hello2: closes
                writer.close()
                return
            cn, sn = line[7:].split(" ")[0], secrets.token_hex(16)
            writer.write(f"chal {sn}\n".encode())
            await writer.drain()
            auth = (await reader.readline()).decode().strip()
            cp = auth[5:] if auth.startswith("auth ") else ""
            token = next((t for t in self.tokens if hmac.compare_digest(_proof(t, "c", cn, sn), cp)), None)
            if self.impostor:
                token = "0" * 32                        # pretends to accept, cannot prove
            if token is None:
                writer.write(b"denied\n")
                await writer.drain()
                writer.close()
                return
            self.v2_logins += 1
            ok = f"ok {HUB_ID} {_proof(token, 's', sn, cn, HUB_ID)}\n"
        else:
            token = line[6:] if line.startswith("hello ") else ""
            if token:
                self.plain_tokens.append(token)
            if token and self.pairing_open:
                self.tokens.add(token)
            if token not in self.tokens:
                writer.write(b"denied\n")
                await writer.drain()
                writer.close()
                return
        writer.write(ok.encode())
        self.writers.append(writer)
        for cmd, payload in self.on_hello:
            writer.write(f"{cmd:02x} {payload.hex()}\n".encode())
        await writer.drain()
        while raw := await reader.readline():
            cmd, _, body = raw.decode().strip().partition(" ")
            if cmd == "00":
                text = bytes.fromhex(body).decode()
                self.requests.append(text)
                if text.startswith("hub "):         # mode requests are answered with the new mode, like the firmware
                    key, _, value = text[4:].partition("=")
                    if key == "mode":
                        self.mode[0] = 1 if value == "standalone" else 0
                    elif key == "fallback":
                        self.mode[2] = 1 if value == "1" else 0
                    elif key == "preoff":              # answered with the boiler settings, like the firmware
                        self.preheat_off = int(value)
                        for w in self.writers:
                            w.write(f"0c {bytes([self.preheat_off]).hex()}\n".encode())
                        continue
                    for w in self.writers:
                        w.write(f"0b {bytes(self.mode).hex()}\n".encode())

    async def push(self, cmd: int, payload: bytes) -> None:
        for w in self.writers:
            w.write(f"{cmd:02x} {payload.hex()}\n".encode())
            await w.drain()
        await asyncio.sleep(0.05)       # let the client read it

    async def push_state(self, kind: int, mac: str, blob_hex: str) -> None:
        await self.push(0x01, bytes([kind]) + bytes.fromhex(mac) + bytes.fromhex(blob_hex))

    def seed(self, thermostat: bool = True) -> None:
        """What a hub with one boiler, one thermostat (three rooms), a room sensor and a gas meter sends on connect.
        thermostat=False: the boiler alone (its own controller regulates the room: floor/room control is offered)."""
        import struct

        links = bytes([6]) + bytes.fromhex(BOILER) + (bytes([9]) + bytes.fromhex(THERMO) if thermostat else b"")
        self.on_hello = [
            (0x02, links),
            (0x05, bytes([3, 0, 0])),
            (0x01, bytes([6]) + bytes.fromhex(BOILER) + bytes.fromhex(F06_ON)),
            *([(0x01, bytes([9]) + bytes.fromhex(THERMO) + bytes.fromhex(f09()))] if thermostat else []),
            (0x03, struct.pack("<hh", 2534, 3410)),
            (0x04, struct.pack("<5I", 1504, 0xFFFFFFFF, 210, 5190, 857)),
            (0x0B, bytes(self.mode)),
            (0x0C, bytes([self.preheat_off])),
        ]


@pytest.fixture
async def fake_hub():
    hub = FakeHub()
    await hub.start()
    yield hub
    await hub.stop()


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    yield
