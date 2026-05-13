#include "dusk/net/net.h"

#include "dusk/logging.h"
#include "dusk/net/api_client.h"
#include "dusk/net/replication.h"
#include "dusk/net/save_codec.h"
#include "dusk/net/transport.h"
#include "dusk/net/wire.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dusk::net {

namespace {

std::unique_ptr<Transport> g_transport;
TransportState g_lastTransportState = TransportState::Disconnected;

std::atomic<std::uint64_t> g_framesReceived{0};
std::atomic<std::uint64_t> g_framesSent{0};
std::atomic<std::uint64_t> g_framesDropped{0};
std::atomic<std::uint64_t> g_simTicks{0};
std::atomic<std::uint8_t> g_playerIndex{0xFF};
std::atomic<HandshakeError> g_handshakeError{HandshakeError::None};

// Guards strings that the sim thread writes and the UI thread reads.
std::mutex g_stringMutex;
std::string g_sessionCode;
std::string g_lastErrorMessage;

// Pending Hello to emit when the transport first hits Connected.
bool g_hasPendingHello = false;
wire::Hello g_pendingHello{};

ConnectionState ToConnectionState(TransportState s) {
    switch (s) {
    case TransportState::Disconnected: return ConnectionState::Disconnected;
    case TransportState::Connecting:   return ConnectionState::Connecting;
    case TransportState::Connected:    return ConnectionState::Connected;
    case TransportState::Closing:      return ConnectionState::Closing;
    }
    return ConnectionState::Disconnected;
}

void SetString(std::string& field, std::string value) {
    std::lock_guard<std::mutex> lock(g_stringMutex);
    field = std::move(value);
}

std::string ReadString(const std::string& field) {
    std::lock_guard<std::mutex> lock(g_stringMutex);
    return field;
}

HandshakeError MapApiError(ApiErrorKind k) {
    switch (k) {
    case ApiErrorKind::Network:           return HandshakeError::Network;
    case ApiErrorKind::IsoMismatch:       return HandshakeError::IsoMismatch;
    case ApiErrorKind::SessionNotFound:   return HandshakeError::SessionNotFound;
    case ApiErrorKind::SessionFull:       return HandshakeError::SessionFull;
    case ApiErrorKind::BadRequest:        return HandshakeError::BadResponse;
    case ApiErrorKind::UnexpectedResponse: return HandshakeError::BadResponse;
    case ApiErrorKind::Internal:          return HandshakeError::Internal;
    }
    return HandshakeError::Internal;
}

void ResetHandshakeState() {
    g_playerIndex.store(0xFF, std::memory_order_relaxed);
    g_handshakeError.store(HandshakeError::None, std::memory_order_relaxed);
    SetString(g_lastErrorMessage, {});
}

void OpenTransport(const std::string& base_url, const std::string& session_code,
                   const std::string& token) {
    // base_url is http(s)://host:port — swap the scheme for ws(s):// to build
    // the WebSocket URL.
    std::string ws_url = base_url;
    if (ws_url.rfind("https://", 0) == 0) {
        ws_url.replace(0, 8, "wss://");
    } else if (ws_url.rfind("http://", 0) == 0) {
        ws_url.replace(0, 7, "ws://");
    }
    ws_url += "/v1/session/" + session_code + "/ws?token=" + token;
    g_transport->connect(ws_url);
}

bool ValidIsoHashSize(std::span<const std::uint8_t> h) {
    return h.size() == wire::kIsoHashLen;
}

}  // namespace

bool Init() {
    if (g_transport) return true;
    if (!wire::RunSelfTest()) {
        DuskLog.error("dusk::net: wire-format self-test FAILED — handshake will not succeed. "
                      "Client and server wire codecs are out of sync.");
    }
    if (!save_codec::RunSelfTest()) {
        DuskLog.error("dusk::net: save-codec self-test FAILED — save sync will be wrong. "
                      "Client and server save codecs are out of sync.");
    }
    g_transport = MakeWebSocketTransport();
    DuskLog.debug("dusk::net::Init");
    return static_cast<bool>(g_transport);
}

void Shutdown() {
    if (!g_transport) return;
    g_transport->disconnect();
    g_transport.reset();
    DuskLog.debug("dusk::net::Shutdown");
}

bool HostSession(const HostSessionConfig& cfg) {
    if (!g_transport) return false;
    g_transport->disconnect();
    ResetHandshakeState();

    DuskLog.debug("dusk::net::HostSession base={}", cfg.base_url);
    auto result = ApiCreateSession(cfg.base_url, cfg.display_name, cfg.iso_hash);
    if (!result.ok()) {
        const auto& err = result.error();
        g_handshakeError.store(MapApiError(err.kind), std::memory_order_relaxed);
        SetString(g_lastErrorMessage, err.message);
        DuskLog.warn("dusk::net::HostSession failed: {}", err.message);
        return false;
    }
    const auto& resp = result.value();
    SetString(g_sessionCode, resp.session_code);

    // Build the Hello we'll emit once the WS reaches Connected.
    wire::Hello hello;
    hello.protocol_version = wire::kProtocolVersion;
    hello.iso_hash = cfg.iso_hash;
    std::memcpy(hello.session_code.data(), resp.session_code.data(),
                std::min<std::size_t>(resp.session_code.size(), wire::kSessionCodeLen));
    hello.intent = wire::HelloIntent::Host;
    hello.display_name = cfg.display_name;
    g_pendingHello = std::move(hello);
    g_hasPendingHello = true;

    OpenTransport(cfg.base_url, resp.session_code, resp.host_token);
    return true;
}

bool JoinSession(const JoinSessionConfig& cfg) {
    if (!g_transport) return false;
    if (cfg.session_code.size() != wire::kSessionCodeLen) {
        SetString(g_lastErrorMessage, "session code must be 6 chars");
        g_handshakeError.store(HandshakeError::BadResponse, std::memory_order_relaxed);
        return false;
    }
    g_transport->disconnect();
    ResetHandshakeState();

    DuskLog.debug("dusk::net::JoinSession code={}", cfg.session_code);
    auto result = ApiJoinSession(cfg.base_url, cfg.session_code,
                                 cfg.display_name, cfg.iso_hash);
    if (!result.ok()) {
        const auto& err = result.error();
        g_handshakeError.store(MapApiError(err.kind), std::memory_order_relaxed);
        SetString(g_lastErrorMessage, err.message);
        DuskLog.warn("dusk::net::JoinSession failed: {}", err.message);
        return false;
    }
    const auto& resp = result.value();
    SetString(g_sessionCode, cfg.session_code);

    wire::Hello hello;
    hello.protocol_version = wire::kProtocolVersion;
    hello.iso_hash = cfg.iso_hash;
    std::memcpy(hello.session_code.data(), cfg.session_code.data(), wire::kSessionCodeLen);
    hello.intent = wire::HelloIntent::Guest;
    hello.display_name = cfg.display_name;
    g_pendingHello = std::move(hello);
    g_hasPendingHello = true;

    OpenTransport(cfg.base_url, cfg.session_code, resp.guest_token);
    return true;
}

void Disconnect() {
    if (!g_transport) return;
    g_transport->disconnect();
    g_hasPendingHello = false;
    ResetHandshakeState();
    SetString(g_sessionCode, {});
    replication::DespawnPuppet();
}

void Tick() {
    g_simTicks.fetch_add(1, std::memory_order_relaxed);
    if (!g_transport) return;

    // Send pending Hello on the rising edge of Connected.
    const auto state = g_transport->state();
    if (state == TransportState::Connected
        && g_lastTransportState != TransportState::Connected
        && g_hasPendingHello)
    {
        auto bytes = wire::Encode(wire::Frame{g_pendingHello});
        if (g_transport->send(bytes)) {
            g_framesSent.fetch_add(1, std::memory_order_relaxed);
            g_hasPendingHello = false;
            DuskLog.debug("dusk::net: Hello sent ({} bytes)", bytes.size());
        }
    }
    g_lastTransportState = state;

    // Drain inbound frames and dispatch known wire packets.
    std::vector<std::uint8_t> frame_bytes;
    while (g_transport->try_recv(frame_bytes)) {
        g_framesReceived.fetch_add(1, std::memory_order_relaxed);
        auto decoded = wire::Decode(frame_bytes);
        if (!decoded) {
            // Could be a routed peer payload not yet protocol-typed (M3+).
            // Keep counting but don't error out the handshake.
            continue;
        }
        std::visit(
            [](auto const& f) {
                using T = std::decay_t<decltype(f)>;
                if constexpr (std::is_same_v<T, wire::HelloAck>) {
                    g_playerIndex.store(f.player_index, std::memory_order_relaxed);
                    g_handshakeError.store(HandshakeError::None, std::memory_order_relaxed);
                    DuskLog.debug("dusk::net: HelloAck player_index={}", f.player_index);
                } else if constexpr (std::is_same_v<T, wire::ErrorIsoMismatch>) {
                    g_handshakeError.store(HandshakeError::IsoMismatch,
                                           std::memory_order_relaxed);
                    SetString(g_lastErrorMessage, "ISO hash mismatch");
                    if (g_transport) g_transport->disconnect();
                } else if constexpr (std::is_same_v<T, wire::ErrorProtocolVersion>) {
                    g_handshakeError.store(HandshakeError::ProtocolVersion,
                                           std::memory_order_relaxed);
                    SetString(g_lastErrorMessage,
                              "protocol version mismatch (server="
                              + std::to_string(f.server_version) + ")");
                    if (g_transport) g_transport->disconnect();
                } else if constexpr (std::is_same_v<T, wire::ErrorSessionNotFound>) {
                    g_handshakeError.store(HandshakeError::SessionNotFound,
                                           std::memory_order_relaxed);
                    SetString(g_lastErrorMessage, "session not found");
                    if (g_transport) g_transport->disconnect();
                } else if constexpr (std::is_same_v<T, wire::ErrorSessionFull>) {
                    g_handshakeError.store(HandshakeError::SessionFull,
                                           std::memory_order_relaxed);
                    SetString(g_lastErrorMessage, "session full");
                    if (g_transport) g_transport->disconnect();
                } else if constexpr (std::is_same_v<T, wire::PlayerPose>) {
                    replication::OnPeerPose(f);
                } else if constexpr (std::is_same_v<T, wire::PlayerAnim>) {
                    replication::OnPeerAnim(f);
                } else if constexpr (std::is_same_v<T, wire::SceneAnnounce>) {
                    replication::OnPeerSceneAnnounce(f);
                }
                // Other variants (Hello / ErrorSaveCommitConflict) aren't
                // expected on the client side in M2.
            },
            *decoded);
    }

    replication::Tick();
}

bool Send(std::span<const std::uint8_t> frame) {
    if (!g_transport) return false;
    if (g_transport->send(frame)) {
        g_framesSent.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_framesDropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool IsConnected() {
    return g_transport && g_transport->state() == TransportState::Connected;
}

Stats GetStats() {
    Stats s;
    s.state = g_transport ? ToConnectionState(g_transport->state())
                          : ConnectionState::Disconnected;
    s.framesReceived = g_framesReceived.load(std::memory_order_relaxed);
    s.framesSent = g_framesSent.load(std::memory_order_relaxed);
    s.framesDropped = g_framesDropped.load(std::memory_order_relaxed);
    s.inboundQueueDepth = 0;
    s.simTicks = g_simTicks.load(std::memory_order_relaxed);
    s.playerIndex = g_playerIndex.load(std::memory_order_relaxed);
    s.handshakeError = g_handshakeError.load(std::memory_order_relaxed);
    s.sessionCode = ReadString(g_sessionCode);
    s.lastErrorMessage = ReadString(g_lastErrorMessage);
    return s;
}

}  // namespace dusk::net
