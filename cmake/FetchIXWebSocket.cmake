# IXWebSocket — WebSocket client used by the dusk::net multiplayer subsystem.
# Selected during the M1 spike (see docs/multiplayer.md).
#
# TLS is intentionally OFF for the v1 MVP — the plan documents wss:// support
# via a reverse proxy (caddy/nginx/traefik) rather than terminating in-process,
# and the M7 polish pass will revisit if first-party wss:// is needed.

set(IXWEBSOCKET_USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_WS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL OFF CACHE BOOL "" FORCE)
set(USE_MBED_TLS OFF CACHE BOOL "" FORCE)
set(USE_SECURE_TRANSPORT OFF CACHE BOOL "" FORCE)
# Skip permessage-deflate; we don't negotiate the extension and we'd
# otherwise need a system zlib (the codebase currently bundles only zstd).
set(USE_ZLIB OFF CACHE BOOL "" FORCE)

message(STATUS "dusk: Fetching IXWebSocket")
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v11.4.5
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(ixwebsocket)
