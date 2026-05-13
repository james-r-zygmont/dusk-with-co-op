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
inline constexpr std::size_t kStageNameLen = 8;

// `mTransformStatus` in `dSv_player_status_a_c`; per-player overlay.
inline constexpr std::uint8_t kTransformHuman = 0;
inline constexpr std::uint8_t kTransformWolf = 1;

inline constexpr std::uint8_t kHeldItemNone = 0xFF;

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

    // M3 replication packets — peer ↔ peer via the relay.
    PlayerPose = 16,
    PlayerAnim = 17,
    SceneAnnounce = 18,
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

// One pose snapshot from a peer's daAlink_c. Source fields on fopAc_ac_c:
// current.pos @ 0x4D0, shape_angle @ 0x4E4, speed @ 0x4F8, speedF @ 0x52C.
struct PlayerPose {
    std::uint32_t tick = 0;
    std::array<std::uint8_t, kStageNameLen> stage{};
    std::uint8_t room = 0;
    float pos[3] = {0.f, 0.f, 0.f};
    std::int16_t shape_angle[3] = {0, 0, 0};
    float speed[3] = {0.f, 0.f, 0.f};
    float speed_f = 0.f;
};

// Animation snapshot. Sent on change; the receiver advances `frame` itself
// between updates so a dropped packet doesn't freeze the puppet.
struct PlayerAnim {
    std::uint16_t anim_id = 0;
    float frame = 0.f;
    std::uint8_t transform = kTransformHuman;
    std::uint8_t held_item = kHeldItemNone;
};

// Emitted when our scene-request reaches the Done phase; the relay caches
// the latest per slot and flips co-location on when host's stage+room match
// guest's.
struct SceneAnnounce {
    std::array<std::uint8_t, kStageNameLen> stage{};
    std::uint8_t room = 0;
    std::uint8_t spawn = 0;
};

using Frame = std::variant<
    Hello,
    HelloAck,
    ErrorIsoMismatch,
    ErrorProtocolVersion,
    ErrorSessionNotFound,
    ErrorSessionFull,
    ErrorSaveCommitConflict,
    PlayerPose,
    PlayerAnim,
    SceneAnnounce>;

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
