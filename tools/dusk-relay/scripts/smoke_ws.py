"""
End-to-end M2 smoke test: REST session lifecycle + WebSocket Hello handshake
+ bidirectional frame routing through the relay.

Requires: pip install websockets
Run against a dusk-relay listening on 127.0.0.1:7783 (override with --port).
"""

import argparse
import asyncio
import struct
import sys
import urllib.request
import json

import websockets

PROTOCOL_VERSION = 1
TAG_HELLO = 1
TAG_HELLO_ACK = 2
TAG_ERROR_ISO_MISMATCH = 3
TAG_ERROR_PROTOCOL_VERSION = 4
TAG_ERROR_SESSION_NOT_FOUND = 5
TAG_ERROR_SESSION_FULL = 6


def encode_hello(version: int, iso_hash: bytes, code: bytes, intent: int, name: str) -> bytes:
    assert len(iso_hash) == 16
    assert len(code) == 6
    name_bytes = name.encode("utf-8")
    return (
        bytes([TAG_HELLO])
        + struct.pack(">H", version)
        + iso_hash
        + code
        + bytes([intent])
        + struct.pack(">H", len(name_bytes))
        + name_bytes
    )


def decode_frame(b: bytes) -> dict:
    if not b:
        raise ValueError("empty frame")
    tag = b[0]
    if tag == TAG_HELLO_ACK:
        if len(b) != 1 + 2 + 1 + 8:
            raise ValueError(f"HelloAck wrong length: {len(b)}")
        version = struct.unpack(">H", b[1:3])[0]
        player_index = b[3]
        server_time = struct.unpack(">Q", b[4:12])[0]
        return {"kind": "HelloAck", "protocol_version": version,
                "player_index": player_index, "server_time_ms": server_time}
    if tag == TAG_ERROR_ISO_MISMATCH:
        return {"kind": "ErrorIsoMismatch", "server_hash": b[1:17].hex()}
    if tag == TAG_ERROR_PROTOCOL_VERSION:
        return {"kind": "ErrorProtocolVersion",
                "server_version": struct.unpack(">H", b[1:3])[0]}
    if tag == TAG_ERROR_SESSION_NOT_FOUND:
        return {"kind": "ErrorSessionNotFound"}
    if tag == TAG_ERROR_SESSION_FULL:
        return {"kind": "ErrorSessionFull"}
    return {"kind": f"tag_{tag}", "body": b[1:].hex()}


def post_json(url: str, payload: dict) -> dict:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read())


async def run(base: str, ws_base: str) -> None:
    iso = bytes.fromhex("00112233445566778899aabbccddeeff")

    print("[smoke] POST /v1/sessions")
    create = post_json(f"{base}/v1/sessions",
                       {"display_name": "Alice", "iso_hash": iso.hex()})
    code = create["session_code"].encode("ascii")
    host_token = create["host_token"]
    print(f"        code={create['session_code']} player_index={create['player_index']}")

    print("[smoke] POST /v1/sessions/{code}/join")
    join = post_json(f"{base}/v1/sessions/{create['session_code']}/join",
                     {"display_name": "Bob", "iso_hash": iso.hex()})
    guest_token = join["guest_token"]
    print(f"        guest_index={join['player_index']}")

    host_uri = f"{ws_base}/v1/session/{create['session_code']}/ws?token={host_token}"
    guest_uri = f"{ws_base}/v1/session/{create['session_code']}/ws?token={guest_token}"

    async with websockets.connect(host_uri) as host_ws, \
               websockets.connect(guest_uri) as guest_ws:

        await host_ws.send(encode_hello(PROTOCOL_VERSION, iso, code, intent=0, name="Alice"))
        host_ack = decode_frame(await host_ws.recv())
        print(f"[smoke] host HelloAck: {host_ack}")
        assert host_ack["kind"] == "HelloAck" and host_ack["player_index"] == 0

        await guest_ws.send(encode_hello(PROTOCOL_VERSION, iso, code, intent=1, name="Bob"))
        guest_ack = decode_frame(await guest_ws.recv())
        print(f"[smoke] guest HelloAck: {guest_ack}")
        assert guest_ack["kind"] == "HelloAck" and guest_ack["player_index"] == 1

        # Host → Guest
        payload_h2g = b"\xfe\x01ping-from-host"
        await host_ws.send(payload_h2g)
        recv_at_guest = await asyncio.wait_for(guest_ws.recv(), timeout=2.0)
        print(f"[smoke] guest received host payload (len={len(recv_at_guest)})")
        assert recv_at_guest == payload_h2g, f"mismatch: {recv_at_guest!r} vs {payload_h2g!r}"

        # Guest → Host
        payload_g2h = b"\xfe\x02ping-from-guest"
        await guest_ws.send(payload_g2h)
        recv_at_host = await asyncio.wait_for(host_ws.recv(), timeout=2.0)
        print(f"[smoke] host received guest payload (len={len(recv_at_host)})")
        assert recv_at_host == payload_g2h, f"mismatch: {recv_at_host!r} vs {payload_g2h!r}"

    # Wrong ISO hash → typed error frame
    print("[smoke] wrong-ISO Hello (expect ErrorIsoMismatch)")
    wrong_iso = b"\xff" * 16
    async with websockets.connect(host_uri) as ws:
        await ws.send(encode_hello(PROTOCOL_VERSION, wrong_iso, code, intent=0, name="Alice"))
        err = decode_frame(await ws.recv())
        print(f"        got: {err}")
        assert err["kind"] == "ErrorIsoMismatch"

    print("[smoke] OK")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=7783)
    args = parser.parse_args()
    base = f"http://{args.host}:{args.port}"
    ws_base = f"ws://{args.host}:{args.port}"
    try:
        asyncio.run(run(base, ws_base))
    except AssertionError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        sys.exit(1)
