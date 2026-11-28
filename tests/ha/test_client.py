"""The LAN client's authentication: the token never goes over the wire once the hub has proved itself (v2)."""

from __future__ import annotations

import asyncio

import pytest

from custom_components.lynnai_hub.client import HubClient, try_pair

from .conftest import HUB_ID


@pytest.fixture(autouse=True)
def _sockets(socket_enabled):
    yield


async def _run_client(fake_hub, token: str, v2_seen: bool, seconds: float = 0.6):
    events: list[tuple[bool, str | None]] = []
    verified: list[tuple[str, int]] = []
    client = HubClient("127.0.0.1", fake_hub.port, token, HUB_ID, lambda c, p: None,
                       lambda up, why: events.append((up, why)), None, v2_seen=v2_seen,
                       on_verified=lambda h, p: verified.append((h, p)))
    client.start()
    await asyncio.sleep(seconds)
    await client.stop()
    return client, events, verified


async def test_v2_login_never_sends_the_token(fake_hub):
    fake_hub.pairing_open = True
    token = "0123456789abcdef0123456789abcdef"
    hub_id, v2 = await try_pair("127.0.0.1", fake_hub.port, token)
    assert hub_id == HUB_ID and v2
    fake_hub.plain_tokens.clear()                       # the v1 pairing itself sent it once
    client, events, verified = await _run_client(fake_hub, token, v2_seen=True)
    assert events and events[0][0] is True and fake_hub.v2_logins >= 2
    assert fake_hub.plain_tokens == []
    assert verified == [("127.0.0.1", fake_hub.port)]   # reported once so the address is kept


async def test_impostor_gets_nothing_after_v2(fake_hub):
    """Something on the LAN answers as our hub: a wrong proof, or 'denied', must not make us send the token."""
    token = "fedcba9876543210fedcba9876543210"
    fake_hub.impostor = True
    _client, events, _ = await _run_client(fake_hub, token, v2_seen=True)
    assert not any(up for up, _ in events) and fake_hub.plain_tokens == []
    fake_hub.impostor = False                           # now it just says "denied" (does not know the token)
    _client, events, _ = await _run_client(fake_hub, token, v2_seen=True, seconds=0.6)
    assert not any(up for up, _ in events) and fake_hub.plain_tokens == []


@pytest.fixture
def _fast_retry(monkeypatch):
    from custom_components.lynnai_hub import client as mod

    monkeypatch.setattr(mod, "BACKOFF_FIRST", 0.05)
    monkeypatch.setattr(mod, "BACKOFF_MAX", 0.1)
    monkeypatch.setattr(mod, "BACKOFF_REJECTED", 0.05)


async def _closed_port() -> int:
    server = await asyncio.start_server(lambda r, w: None, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    server.close()
    await server.wait_closed()
    return port


async def test_no_plain_token_to_an_address_found_by_ssdp(fake_hub, _fast_retry):
    """The saved address stops answering and SSDP points elsewhere: something there acting like old firmware
    (or saying 'denied') must not get the token, even though this install never saw v2."""
    token = "00112233445566778899aabbccddeeff"
    home = await _closed_port()
    found: list[int] = []

    async def find_host():
        found.append(1)
        return ("127.0.0.1", fake_hub.port)

    for legacy in (True, False):                        # old firmware imitation, then plain 'denied'
        fake_hub.legacy = legacy
        found.clear()
        client = HubClient("127.0.0.1", home, token, HUB_ID, lambda c, p: None, lambda up, why: None,
                           find_host, v2_seen=False)
        client.start()
        await asyncio.sleep(0.8)
        await client.stop()
        assert found and fake_hub.plain_tokens == []


async def test_old_firmware_at_the_saved_address_still_gets_v1(fake_hub, _fast_retry):
    """Hub firmware before v2 at the address saved in the entry: v1 keeps working there."""
    token = "ffeeddccbbaa99887766554433221100"
    fake_hub.legacy = True
    fake_hub.tokens.add(token)
    _client, events, _ = await _run_client(fake_hub, token, v2_seen=False, seconds=0.8)
    assert token in fake_hub.plain_tokens
    assert any(up for up, _ in events)
