#include "dusk/net/wire.h"

#include <cstring>

namespace dusk::net::wire {

namespace {

inline void write_u8(std::vector<std::uint8_t>& buf, std::uint8_t v) {
    buf.push_back(v);
}

inline void write_u16_be(std::vector<std::uint8_t>& buf, std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>(v >> 8));
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

inline void write_i16_be(std::vector<std::uint8_t>& buf, std::int16_t v) {
    write_u16_be(buf, static_cast<std::uint16_t>(v));
}

inline void write_u32_be(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
    }
}

inline void write_u64_be(std::vector<std::uint8_t>& buf, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        buf.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
    }
}

inline void write_f32_be(std::vector<std::uint8_t>& buf, float v) {
    std::uint32_t bits;
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::memcpy(&bits, &v, sizeof(bits));
    write_u32_be(buf, bits);
}

inline void write_bytes(std::vector<std::uint8_t>& buf, std::span<const std::uint8_t> src) {
    buf.insert(buf.end(), src.begin(), src.end());
}

inline bool read_u8(std::span<const std::uint8_t> bytes, std::size_t& cursor, std::uint8_t& out) {
    if (cursor + 1 > bytes.size()) return false;
    out = bytes[cursor++];
    return true;
}

inline bool read_u16_be(std::span<const std::uint8_t> bytes, std::size_t& cursor, std::uint16_t& out) {
    if (cursor + 2 > bytes.size()) return false;
    out = static_cast<std::uint16_t>((bytes[cursor] << 8) | bytes[cursor + 1]);
    cursor += 2;
    return true;
}

inline bool read_i16_be(std::span<const std::uint8_t> bytes, std::size_t& cursor, std::int16_t& out) {
    std::uint16_t u;
    if (!read_u16_be(bytes, cursor, u)) return false;
    out = static_cast<std::int16_t>(u);
    return true;
}

inline bool read_u32_be(std::span<const std::uint8_t> bytes, std::size_t& cursor, std::uint32_t& out) {
    if (cursor + 4 > bytes.size()) return false;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v = (v << 8) | bytes[cursor + i];
    out = v;
    cursor += 4;
    return true;
}

inline bool read_f32_be(std::span<const std::uint8_t> bytes, std::size_t& cursor, float& out) {
    std::uint32_t bits;
    if (!read_u32_be(bytes, cursor, bits)) return false;
    std::memcpy(&out, &bits, sizeof(out));
    return true;
}

inline bool read_u64_be(std::span<const std::uint8_t> bytes, std::size_t& cursor, std::uint64_t& out) {
    if (cursor + 8 > bytes.size()) return false;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | bytes[cursor + i];
    }
    out = v;
    cursor += 8;
    return true;
}

inline bool read_array(std::span<const std::uint8_t> bytes,
                       std::size_t& cursor,
                       std::span<std::uint8_t> out) {
    if (cursor + out.size() > bytes.size()) return false;
    std::memcpy(out.data(), bytes.data() + cursor, out.size());
    cursor += out.size();
    return true;
}

inline bool read_string(std::span<const std::uint8_t> bytes,
                        std::size_t& cursor,
                        std::string& out) {
    std::uint16_t len = 0;
    if (!read_u16_be(bytes, cursor, len)) return false;
    if (cursor + len > bytes.size()) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data() + cursor), len);
    cursor += len;
    return true;
}

}  // namespace

std::vector<std::uint8_t> Encode(const Frame& frame) {
    std::vector<std::uint8_t> buf;
    buf.reserve(64);

    std::visit(
        [&](auto const& p) {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, Hello>) {
                write_u8(buf, static_cast<std::uint8_t>(Tag::Hello));
                write_u16_be(buf, p.protocol_version);
                write_bytes(buf, p.iso_hash);
                write_bytes(buf, p.session_code);
                write_u8(buf, static_cast<std::uint8_t>(p.intent));
                // String: u16 BE length, then bytes.
                const auto len = static_cast<std::uint16_t>(p.display_name.size());
                write_u16_be(buf, len);
                buf.insert(buf.end(), p.display_name.begin(), p.display_name.end());
            } else if constexpr (std::is_same_v<T, HelloAck>) {
                write_u8(buf, static_cast<std::uint8_t>(Tag::HelloAck));
                write_u16_be(buf, p.protocol_version);
                write_u8(buf, p.player_index);
                write_u64_be(buf, p.server_time_ms);
            } else if constexpr (std::is_same_v<T, ErrorIsoMismatch>) {
                write_u8(buf, static_cast<std::uint8_t>(Tag::ErrorIsoMismatch));
                write_bytes(buf, p.server_hash);
            } else if constexpr (std::is_same_v<T, ErrorProtocolVersion>) {
                write_u8(buf, static_cast<std::uint8_t>(Tag::ErrorProtocolVersion));
                write_u16_be(buf, p.server_version);
            } else if constexpr (std::is_same_v<T, ErrorSessionNotFound>) {
                write_u8(buf, static_cast<std::uint8_t>(Tag::ErrorSessionNotFound));
            } else if constexpr (std::is_same_v<T, ErrorSessionFull>) {
                write_u8(buf, static_cast<std::uint8_t>(Tag::ErrorSessionFull));
            } else if constexpr (std::is_same_v<T, ErrorSaveCommitConflict>) {
                write_u8(buf, static_cast<std::uint8_t>(Tag::ErrorSaveCommitConflict));
            } else if constexpr (std::is_same_v<T, PlayerPose>) {
                write_u8(buf, static_cast<std::uint8_t>(Tag::PlayerPose));
                write_u32_be(buf, p.tick);
                write_bytes(buf, p.stage);
                write_u8(buf, p.room);
                for (float v : p.pos) write_f32_be(buf, v);
                for (std::int16_t v : p.shape_angle) write_i16_be(buf, v);
                for (float v : p.speed) write_f32_be(buf, v);
                write_f32_be(buf, p.speed_f);
            } else if constexpr (std::is_same_v<T, PlayerAnim>) {
                write_u8(buf, static_cast<std::uint8_t>(Tag::PlayerAnim));
                write_u16_be(buf, p.anim_id);
                write_f32_be(buf, p.frame);
                write_u8(buf, p.transform);
                write_u8(buf, p.held_item);
            } else if constexpr (std::is_same_v<T, SceneAnnounce>) {
                write_u8(buf, static_cast<std::uint8_t>(Tag::SceneAnnounce));
                write_bytes(buf, p.stage);
                write_u8(buf, p.room);
                write_u8(buf, p.spawn);
            }
        },
        frame);

    return buf;
}

std::optional<Frame> Decode(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return std::nullopt;

    std::size_t cursor = 0;
    std::uint8_t tag_byte = 0;
    if (!read_u8(bytes, cursor, tag_byte)) return std::nullopt;

    auto consumed_all = [&](Frame frame) -> std::optional<Frame> {
        if (cursor != bytes.size()) return std::nullopt;  // trailing bytes
        return frame;
    };

    switch (static_cast<Tag>(tag_byte)) {
    case Tag::Hello: {
        Hello h;
        if (!read_u16_be(bytes, cursor, h.protocol_version)) return std::nullopt;
        if (!read_array(bytes, cursor, h.iso_hash)) return std::nullopt;
        if (!read_array(bytes, cursor, h.session_code)) return std::nullopt;
        std::uint8_t intent_byte = 0;
        if (!read_u8(bytes, cursor, intent_byte)) return std::nullopt;
        if (intent_byte > 1) return std::nullopt;
        h.intent = static_cast<HelloIntent>(intent_byte);
        if (!read_string(bytes, cursor, h.display_name)) return std::nullopt;
        return consumed_all(std::move(h));
    }
    case Tag::HelloAck: {
        HelloAck a;
        if (!read_u16_be(bytes, cursor, a.protocol_version)) return std::nullopt;
        if (!read_u8(bytes, cursor, a.player_index)) return std::nullopt;
        if (!read_u64_be(bytes, cursor, a.server_time_ms)) return std::nullopt;
        return consumed_all(a);
    }
    case Tag::ErrorIsoMismatch: {
        ErrorIsoMismatch e;
        if (!read_array(bytes, cursor, e.server_hash)) return std::nullopt;
        return consumed_all(e);
    }
    case Tag::ErrorProtocolVersion: {
        ErrorProtocolVersion e;
        if (!read_u16_be(bytes, cursor, e.server_version)) return std::nullopt;
        return consumed_all(e);
    }
    case Tag::ErrorSessionNotFound:
        return consumed_all(ErrorSessionNotFound{});
    case Tag::ErrorSessionFull:
        return consumed_all(ErrorSessionFull{});
    case Tag::ErrorSaveCommitConflict:
        return consumed_all(ErrorSaveCommitConflict{});
    case Tag::PlayerPose: {
        PlayerPose p;
        if (!read_u32_be(bytes, cursor, p.tick)) return std::nullopt;
        if (!read_array(bytes, cursor, p.stage)) return std::nullopt;
        if (!read_u8(bytes, cursor, p.room)) return std::nullopt;
        for (int i = 0; i < 3; ++i) {
            if (!read_f32_be(bytes, cursor, p.pos[i])) return std::nullopt;
        }
        for (int i = 0; i < 3; ++i) {
            if (!read_i16_be(bytes, cursor, p.shape_angle[i])) return std::nullopt;
        }
        for (int i = 0; i < 3; ++i) {
            if (!read_f32_be(bytes, cursor, p.speed[i])) return std::nullopt;
        }
        if (!read_f32_be(bytes, cursor, p.speed_f)) return std::nullopt;
        return consumed_all(p);
    }
    case Tag::PlayerAnim: {
        PlayerAnim p;
        if (!read_u16_be(bytes, cursor, p.anim_id)) return std::nullopt;
        if (!read_f32_be(bytes, cursor, p.frame)) return std::nullopt;
        if (!read_u8(bytes, cursor, p.transform)) return std::nullopt;
        if (!read_u8(bytes, cursor, p.held_item)) return std::nullopt;
        return consumed_all(p);
    }
    case Tag::SceneAnnounce: {
        SceneAnnounce p;
        if (!read_array(bytes, cursor, p.stage)) return std::nullopt;
        if (!read_u8(bytes, cursor, p.room)) return std::nullopt;
        if (!read_u8(bytes, cursor, p.spawn)) return std::nullopt;
        return consumed_all(p);
    }
    }
    return std::nullopt;  // unknown tag
}

bool RunSelfTest() {
    // Byte fixture: matches tools/dusk-relay/src/wire.rs::tests::hello_byte_pinned.
    Hello h;
    h.protocol_version = kProtocolVersion;
    h.iso_hash.fill(0);
    h.session_code = {'A', 'B', 'C', 'D', 'E', 'F'};
    h.intent = HelloIntent::Host;
    h.display_name = "x";

    const std::array<std::uint8_t, 29> expected = {
        0x01,                                                       // tag = Hello
        0x00, 0x01,                                                 // protocol_version = 1
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,             // iso_hash
        'A', 'B', 'C', 'D', 'E', 'F',                               // session_code
        0x00,                                                       // intent = Host
        0x00, 0x01,                                                 // name length = 1
        'x',                                                        // name
    };

    auto bytes = Encode(Frame{h});
    if (bytes.size() != expected.size()) return false;
    if (std::memcmp(bytes.data(), expected.data(), expected.size()) != 0) return false;

    // Decode round-trip on each variant.
    auto try_roundtrip = [](Frame f) {
        auto b = Encode(f);
        auto d = Decode(b);
        return d.has_value();
    };

    if (!try_roundtrip(HelloAck{kProtocolVersion, 1, 0x0123456789ABCDEFull})) return false;
    if (!try_roundtrip(ErrorIsoMismatch{})) return false;
    if (!try_roundtrip(ErrorProtocolVersion{99})) return false;
    if (!try_roundtrip(ErrorSessionNotFound{})) return false;
    if (!try_roundtrip(ErrorSessionFull{})) return false;
    if (!try_roundtrip(ErrorSaveCommitConflict{})) return false;

    // M3 packets.
    {
        PlayerPose pp;
        pp.tick = 12345;
        pp.stage = {'F', '_', 'S', 'P', '1', '0', '3', 0};
        pp.room = 7;
        pp.pos[0] = 1.0f; pp.pos[1] = -2.5f; pp.pos[2] = 3.75f;
        pp.shape_angle[0] = 0; pp.shape_angle[1] = 0x4000; pp.shape_angle[2] = -0x1234;
        pp.speed[0] = 0.1f; pp.speed[1] = 0.0f; pp.speed[2] = -0.25f;
        pp.speed_f = 4.5f;
        if (!try_roundtrip(pp)) return false;
    }
    if (!try_roundtrip(PlayerAnim{0x0042, 17.5f, kTransformHuman, kHeldItemNone})) return false;
    if (!try_roundtrip(SceneAnnounce{
            {'F', '_', 'S', 'P', '1', '0', '3', 0}, 2, 1})) return false;

    // Byte-pinned PlayerPose: matches wire.rs::tests::player_pose_byte_pinned.
    {
        PlayerPose pp;
        pp.tick = 1;
        pp.stage = {'F', '_', 'S', 'P', '1', '0', '3', 0};
        // all other fields default-constructed to zero
        auto bytes = Encode(Frame{pp});
        if (bytes.size() != 48) return false;
        if (bytes[0] != static_cast<std::uint8_t>(Tag::PlayerPose)) return false;
        const std::array<std::uint8_t, 4> expected_tick = {0x00, 0x00, 0x00, 0x01};
        if (std::memcmp(&bytes[1], expected_tick.data(), 4) != 0) return false;
        const std::array<std::uint8_t, 8> expected_stage = {'F', '_', 'S', 'P', '1', '0', '3', 0};
        if (std::memcmp(&bytes[5], expected_stage.data(), 8) != 0) return false;
        if (bytes[13] != 0) return false;
        for (std::size_t i = 14; i < 48; ++i) {
            if (bytes[i] != 0) return false;
        }
    }

    // Unknown tag should fail to decode.
    if (Decode(std::array<std::uint8_t, 1>{0x99}).has_value()) return false;

    // Trailing bytes should fail to decode.
    std::array<std::uint8_t, 2> trailing = {0x05, 0xAB};  // ErrorSessionNotFound + extra
    if (Decode(trailing).has_value()) return false;

    return true;
}

}  // namespace dusk::net::wire
