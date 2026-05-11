# Co-op multiplayer

Two-player co-op for Dusk. Vertical-slice MVP scope: lobby + position/animation
sync + one shared key item in Ordon Village. See the full plan at
`.claude/plans/multiplayer-plan.md`.

## Build

The feature is gated by the CMake option `DUSK_ENABLE_MULTIPLAYER` (default ON
on Windows/Linux/macOS, OFF on iOS/tvOS/Android). The build pulls
[IXWebSocket](https://github.com/machinezone/IXWebSocket) via FetchContent.

```sh
cmake --preset windows-msvc-relwithdebinfo -DDUSK_ENABLE_MULTIPLAYER=ON
cmake --build --preset windows-msvc-relwithdebinfo
```

## M1 spike decisions

- **Transport.** WebSocket binary frames over TCP, via IXWebSocket (BSD-3-Clause,
  cross-platform, built-in OpenSSL/SecureTransport TLS available if we opt in
  later). Picked over uWebSockets (server-optimised; we're the client) and
  native-per-platform (5–7d cost vs 1–2d for IXWebSocket).
- **TLS.** Disabled in M1 — `ws://` only. `wss://` will land via a reverse
  proxy (caddy/nginx/traefik) terminating TLS in front of the relay; the
  client doesn't speak TLS directly until/unless we revisit in M7. The relay
  itself never terminates TLS.
- **Threading.** IXWebSocket runs its own internal worker thread; we use its
  on-message callback to enqueue binary frames into a `BoundedQueue` that the
  sim thread drains in `dusk::net::Tick()` between `mDoCPd_c::read()` and
  `fapGm_Execute()` (see `src/m_Do/m_Do_main.cpp`).

## Validating M1

1. Install the stub relay's dep: `pip install websockets`.
2. Run the stub: `python tools/dusk-relay-stub/main.py`. It listens on
   `ws://0.0.0.0:7777` and broadcasts every received binary frame to every
   connected client (including the sender).
3. Launch two Dusk instances (same machine is fine for the MVP smoke test).
4. In each instance, once the game has launched, surface the menu bar:
   Settings → **Enable Advanced Settings** (turn on), then press **Shift+F1**.
   Open `Debug → Co-op Debug`. The HUD shows connection state, frame
   counters, and a URL field defaulting to `ws://localhost:7777`.
5. Click **Connect** in each instance. The HUD should flip to `connected`.
6. Click **Send test frame** in one instance. Both HUDs should show
   `Frames in: 1` (the stub broadcasts to all clients, including the sender).
7. Kill the stub relay (Ctrl+C). Both HUDs should flip to `disconnected`
   without crashing.

## Layout

- `include/dusk/net/`, `src/dusk/net/` — client subsystem (transport, packet
  queues, eventually save sync / replication / event hooks).
- `src/dusk/imgui/ImGuiCoopDebug.cpp` — M1 debug HUD.
- `tools/dusk-relay-stub/` — Python stub server used to validate M1 only;
  superseded by `tools/dusk-relay/` (the real Rust relay) at M2.
- `cmake/FetchIXWebSocket.cmake` — third-party dep wiring.
