#ifndef DUSK_NET_WIRE_H
#define DUSK_NET_WIRE_H

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace dusk::net::wire {

// Mirror of the Rust wire format in tools/dusk-relay/src/wire.rs. Byte order
// is big-endian (network order). Each WebSocket binary frame carries one
// logical packet: [tag: u8] [body: tag-specific big-endian bytes].

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kIsoHashLen = 16;
inline constexpr std::size_t kSessionCodeLen = 6;

enum class HelloIntent : std::uint8_t {
    Host = 0,
    Guest = 1,
};

enum class Tag : std::uint8_t {
    Hello = 1,
    HelloAck = 2,
    ErrorIsoMismatch = 3,
    ErrorProtocolVersion = 4,
    ErrorSessionNotFound = 5,
    ErrorSessionFull = 6,
    ErrorSaveCommitConflict = 7,
};

struct Hello {
    std::uint16_t protocol_version = kProtocolVersion;
    std::array<std::uint8_t, kIsoHashLen> iso_hash{};
    std::array<std::uint8_t, kSessionCodeLen> session_code{};
    HelloIntent intent = HelloIntent::Host;
    std::string display_name;
};

struct HelloAck {
    std::uint16_t protocol_version = 0;
    std::uint8_t player_index = 0;
    std::uint64_t server_time_ms = 0;
};

struct ErrorIsoMismatch {
    std::array<std::uint8_t, kIsoHashLen> server_hash{};
};

struct ErrorProtocolVersion {
    std::uint16_t server_version = 0;
};

struct ErrorSessionNotFound {};
struct ErrorSessionFull {};
struct ErrorSaveCommitConflict {};

using Frame = std::variant<
    Hello,
    HelloAck,
    ErrorIsoMismatch,
    ErrorProtocolVersion,
    ErrorSessionNotFound,
    ErrorSessionFull,
    ErrorSaveCommitConflict>;

std::vector<std::uint8_t> Encode(const Frame& frame);

// Returns nullopt for unknown tags, truncated bodies, or trailing bytes.
// (Strict decode mirrors the Rust side's FrameError::TrailingBytes.)
std::optional<Frame> Decode(std::span<const std::uint8_t> bytes);

// Cross-language byte-pin self-test. Asserts that this C++ codec produces
// the same bytes as the Rust `wire::hello_byte_pinned` fixture in
// tools/dusk-relay/src/wire.rs, plus a couple of round-trip checks. Run
// once on Init(); a `false` return means the wire format has drifted
// between client and server and the next handshake will fail.
bool RunSelfTest();

}  // namespace dusk::net::wire

#endif
