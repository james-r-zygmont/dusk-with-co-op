#ifndef DUSK_NET_TRANSPORT_H
#define DUSK_NET_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dusk::net {

enum class TransportState : std::uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Closing,
};

// Abstract realtime channel between client and dusk-relay. The MVP
// implementation is WebSocket-over-TCP; this interface intentionally hides
// that so a future UDP/GameNetworkingSockets swap is drop-in.
class Transport {
public:
    virtual ~Transport() = default;

    virtual bool connect(const std::string& url) = 0;
    virtual void disconnect() = 0;
    virtual TransportState state() const = 0;

    // Sends one binary frame. Returns false if the transport cannot accept the
    // frame right now (disconnected, send queue full, etc.).
    virtual bool send(std::span<const std::uint8_t> frame) = 0;

    // Pops one received frame into `out`. Returns false if no frame is ready.
    virtual bool try_recv(std::vector<std::uint8_t>& out) = 0;
};

// Constructs the default (WebSocket-over-TCP) transport for the MVP. A future
// UDP/GameNetworkingSockets impl will land alongside this one.
std::unique_ptr<Transport> MakeWebSocketTransport();

}  // namespace dusk::net

#endif
