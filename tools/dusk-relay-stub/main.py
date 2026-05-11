"""
M1 stub relay — accepts up to two WebSocket clients and broadcasts every
received binary frame back to every connected client (including the sender).
This lets two Dusk instances ping/pong through the same socket the eventual
Rust dusk-relay (M2) will use, without depending on the real relay yet.

Run:
    pip install websockets
    python tools/dusk-relay-stub/main.py [--port 7777]
"""

import argparse
import asyncio

import websockets


async def handle(ws, connected: set) -> None:
    connected.add(ws)
    print(f"[stub] client connected (total={len(connected)})")
    try:
        async for msg in ws:
            for peer in list(connected):
                try:
                    await peer.send(msg)
                except websockets.ConnectionClosed:
                    pass
    finally:
        connected.discard(ws)
        print(f"[stub] client disconnected (total={len(connected)})")


async def main(port: int) -> None:
    connected: set = set()
    print(f"[stub] listening on ws://0.0.0.0:{port}")

    async def handler(ws):
        await handle(ws, connected)

    async with websockets.serve(handler, "0.0.0.0", port):
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=7777)
    args = parser.parse_args()
    try:
        asyncio.run(main(args.port))
    except KeyboardInterrupt:
        pass
