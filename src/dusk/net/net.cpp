#include "dusk/net/net.h"

#include "dusk/logging.h"
#include "dusk/net/transport.h"

#include <atomic>
#include <memory>
#include <vector>

namespace dusk::net {

namespace {

std::unique_ptr<Transport> g_transport;

std::atomic<std::uint64_t> g_framesReceived{0};
std::atomic<std::uint64_t> g_framesSent{0};
std::atomic<std::uint64_t> g_framesDropped{0};
std::atomic<std::uint64_t> g_simTicks{0};

ConnectionState ToConnectionState(TransportState s) {
    switch (s) {
    case TransportState::Disconnected: return ConnectionState::Disconnected;
    case TransportState::Connecting:   return ConnectionState::Connecting;
    case TransportState::Connected:    return ConnectionState::Connected;
    case TransportState::Closing:      return ConnectionState::Closing;
    }
    return ConnectionState::Disconnected;
}

}  // namespace

bool Init() {
    if (g_transport) return true;
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

void Tick() {
    g_simTicks.fetch_add(1, std::memory_order_relaxed);
    if (!g_transport) return;

    // Drain whatever the worker thread queued up since the last tick. M1
    // exit criteria only require that frames make it through; later milestones
    // will dispatch by packet tag here.
    std::vector<std::uint8_t> frame;
    while (g_transport->try_recv(frame)) {
        g_framesReceived.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Connect(const std::string& url) {
    if (!g_transport) return false;
    return g_transport->connect(url);
}

void Disconnect() {
    if (!g_transport) return;
    g_transport->disconnect();
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
    Stats s{};
    s.state = g_transport ? ToConnectionState(g_transport->state())
                          : ConnectionState::Disconnected;
    s.framesReceived = g_framesReceived.load(std::memory_order_relaxed);
    s.framesSent = g_framesSent.load(std::memory_order_relaxed);
    s.framesDropped = g_framesDropped.load(std::memory_order_relaxed);
    s.inboundQueueDepth = 0;  // The transport drains its own inbound queue on Tick.
    s.simTicks = g_simTicks.load(std::memory_order_relaxed);
    return s;
}

}  // namespace dusk::net
