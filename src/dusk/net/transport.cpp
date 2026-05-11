#include "dusk/net/transport.h"

#include "dusk/logging.h"
#include "dusk/net/queue.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketMessageType.h>

#include <atomic>
#include <string>

namespace dusk::net {

namespace {

// IXWebSocket's net subsystem (Winsock on Windows, no-op elsewhere) needs to be
// reference-counted across transports. M1 only ever creates one transport so
// pairing init/uninit in the ctor/dtor is fine; revisit if we ever need
// multiple instances.

class WebSocketTransport final : public Transport {
public:
    WebSocketTransport() : inbound_(kInboundCapacity) {
        ix::initNetSystem();
        ws_.disableAutomaticReconnection();
        ws_.setOnMessageCallback(
            [this](const ix::WebSocketMessagePtr& msg) { onMessage(msg); });
    }

    ~WebSocketTransport() override {
        disconnect();
        ix::uninitNetSystem();
    }

    bool connect(const std::string& url) override {
        const auto current = state_.load(std::memory_order_acquire);
        if (current == TransportState::Connected || current == TransportState::Connecting) {
            return false;
        }
        ws_.setUrl(url);
        state_.store(TransportState::Connecting, std::memory_order_release);
        ws_.start();
        return true;
    }

    void disconnect() override {
        if (state_.load(std::memory_order_acquire) == TransportState::Disconnected) {
            return;
        }
        state_.store(TransportState::Closing, std::memory_order_release);
        ws_.stop();
        state_.store(TransportState::Disconnected, std::memory_order_release);
    }

    TransportState state() const override {
        return state_.load(std::memory_order_acquire);
    }

    bool send(std::span<const std::uint8_t> frame) override {
        if (state() != TransportState::Connected) {
            return false;
        }
        // IXWebSocket's sendBinary takes `const std::string&`; copy the bytes
        // in. Frames at M1 are tiny (single-digit-byte ping/pong) so the
        // alloc is fine — revisit if profiling shows it as a hotspot.
        std::string buf(reinterpret_cast<const char*>(frame.data()), frame.size());
        return ws_.sendBinary(buf).success;
    }

    bool try_recv(std::vector<std::uint8_t>& out) override {
        if (auto opt = inbound_.try_pop()) {
            out = std::move(*opt);
            return true;
        }
        return false;
    }

private:
    void onMessage(const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Open:
            state_.store(TransportState::Connected, std::memory_order_release);
            DuskLog.debug("dusk::net: WebSocket open");
            break;
        case ix::WebSocketMessageType::Close:
            state_.store(TransportState::Disconnected, std::memory_order_release);
            DuskLog.debug("dusk::net: WebSocket closed (code {})", msg->closeInfo.code);
            break;
        case ix::WebSocketMessageType::Message:
            if (msg->binary) {
                std::vector<std::uint8_t> bytes(msg->str.begin(), msg->str.end());
                if (!inbound_.push(std::move(bytes))) {
                    DuskLog.warn("dusk::net: inbound queue full; dropping frame");
                }
            }
            // Text frames are ignored — the wire protocol is binary-only.
            break;
        case ix::WebSocketMessageType::Error:
            DuskLog.warn("dusk::net: WebSocket error: {}", msg->errorInfo.reason);
            state_.store(TransportState::Disconnected, std::memory_order_release);
            break;
        default:
            break;
        }
    }

    static constexpr std::size_t kInboundCapacity = 256;

    ix::WebSocket ws_;
    std::atomic<TransportState> state_{TransportState::Disconnected};
    BoundedQueue<std::vector<std::uint8_t>> inbound_;
};

}  // namespace

std::unique_ptr<Transport> MakeWebSocketTransport() {
    return std::make_unique<WebSocketTransport>();
}

}  // namespace dusk::net
