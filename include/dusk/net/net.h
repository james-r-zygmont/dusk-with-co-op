#ifndef DUSK_NET_NET_H
#define DUSK_NET_NET_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace dusk::net {

// Lifecycle (called from the existing m_Do_main.cpp init/tick/shutdown sites):
bool Init();
void Shutdown();
void Tick();
bool IsConnected();

// Session lifecycle. Both calls are synchronous (HTTP POST under the hood).
// In M2 they are wired from the ImGui debug HUD; M6 will drive them from the
// RmlUi lobby.
//
// On success the transport is opened against the server's WebSocket endpoint
// and a Hello frame is sent automatically once the WebSocket reaches the
// Connected state. The Stats struct surfaces the HelloAck-supplied player
// index and any typed handshake error.
struct HostSessionConfig {
    std::string base_url;       // e.g. http://localhost:7777
    std::string display_name;
    std::array<std::uint8_t, 16> iso_hash{};
};

struct JoinSessionConfig {
    std::string base_url;
    std::string session_code;   // 6 chars (the alphabet is validated server-side)
    std::string display_name;
    std::array<std::uint8_t, 16> iso_hash{};
};

bool HostSession(const HostSessionConfig& cfg);
bool JoinSession(const JoinSessionConfig& cfg);
void Disconnect();

// Canonical-save sync (M4 chunk 1). HostSession schedules a push and
// JoinSession a pull automatically; these let the debug HUD trigger one on
// demand. The actual REST round-trip + dSv_save_c overwrite happens on the
// sim thread inside Tick() once the session handshake has completed.
void RequestPushCanonicalSave();   // upload local dSv_save_c to the relay
void RequestPullCanonicalSave();   // download + apply the relay's canonical save
std::uint32_t GetCanonicalSaveVersion();  // 0 until a push/pull has succeeded

// Push one binary frame onto the wire (debug HUD only at M2; real packets
// from later milestones go through a typed helper).
bool Send(std::span<const std::uint8_t> frame);

enum class ConnectionState : std::uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Closing,
};

enum class HandshakeError : std::uint8_t {
    None,
    Network,
    IsoMismatch,
    ProtocolVersion,
    SessionNotFound,
    SessionFull,
    BadResponse,
    Internal,
};

struct Stats {
    ConnectionState state = ConnectionState::Disconnected;
    std::uint64_t framesReceived = 0;
    std::uint64_t framesSent = 0;
    std::uint64_t framesDropped = 0;
    std::size_t inboundQueueDepth = 0;
    std::uint64_t simTicks = 0;
    std::string sessionCode;
    std::uint8_t playerIndex = 0xFF;  // 0xFF = unset
    HandshakeError handshakeError = HandshakeError::None;
    std::string lastErrorMessage;
};

Stats GetStats();

}  // namespace dusk::net

#endif
