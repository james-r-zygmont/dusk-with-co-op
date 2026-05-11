#ifndef DUSK_NET_NET_H
#define DUSK_NET_NET_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace dusk::net {

// Initialise the multiplayer subsystem. Safe to call once at startup; returns
// false if a fatal initialisation error occurred (the game should keep running
// in single-player mode in that case).
bool Init();

// Tear the subsystem down. Safe to call once at shutdown.
void Shutdown();

// Drain inbound packet effects onto the sim thread. Must be called once per
// sim tick, between mDoCPd_c::read() and fapGm_Execute(), so that all
// game-state mutations from the network are applied in the same window that
// the rest of the actor framework runs (see m_Do_main.cpp).
void Tick();

// Begin/end the realtime connection to a dusk-relay server. M1 wires these to
// the ImGui debug HUD; later milestones drive them from the lobby UI.
bool Connect(const std::string& url);
void Disconnect();

// Push one binary frame onto the wire. Returns false if not currently
// connected (or the transport refused the frame). M1 uses this for the debug
// HUD's "send test frame" button; later milestones serialise real packets.
bool Send(std::span<const std::uint8_t> frame);

bool IsConnected();

enum class ConnectionState : std::uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Closing,
};

struct Stats {
    ConnectionState state;
    std::uint64_t framesReceived;
    std::uint64_t framesSent;
    std::uint64_t framesDropped;
    std::size_t inboundQueueDepth;
    std::uint64_t simTicks;
};

Stats GetStats();

}  // namespace dusk::net

#endif
