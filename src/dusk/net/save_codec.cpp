#include "dusk/net/save_codec.h"

#include <array>
#include <cstring>

namespace dusk::net::save_codec {

namespace {

inline void write_u16_be(std::vector<std::uint8_t>& buf, std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>(v >> 8));
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
}

inline bool read_u16_be(std::span<const std::uint8_t> bytes,
                        std::size_t& cursor,
                        std::uint16_t& out)
{
    if (cursor + 2 > bytes.size()) return false;
    out = static_cast<std::uint16_t>((bytes[cursor] << 8) | bytes[cursor + 1]);
    cursor += 2;
    return true;
}

}  // namespace

std::vector<std::uint8_t> Encode(const SaveFields& fields, CodecError* out_error) {
    if (out_error) *out_error = CodecError::None;
    std::vector<std::uint8_t> buf;
    buf.reserve(fields.size() * 8);

    for (const auto& [field_id, bytes] : fields) {
        if (bytes.size() > 0xFFFFu) {
            if (out_error) *out_error = CodecError::FieldTooLong;
            return {};
        }
        write_u16_be(buf, field_id);
        write_u16_be(buf, static_cast<std::uint16_t>(bytes.size()));
        buf.insert(buf.end(), bytes.begin(), bytes.end());
    }
    return buf;
}

std::optional<SaveFields> Decode(std::span<const std::uint8_t> blob,
                                 CodecError* out_error)
{
    if (out_error) *out_error = CodecError::None;
    SaveFields out;

    std::size_t cursor = 0;
    while (cursor < blob.size()) {
        if (blob.size() - cursor < 4) {
            if (out_error) *out_error = CodecError::TruncatedHeader;
            return std::nullopt;
        }
        std::uint16_t field_id = 0;
        std::uint16_t len = 0;
        if (!read_u16_be(blob, cursor, field_id)) {
            if (out_error) *out_error = CodecError::TruncatedHeader;
            return std::nullopt;
        }
        if (!read_u16_be(blob, cursor, len)) {
            if (out_error) *out_error = CodecError::TruncatedHeader;
            return std::nullopt;
        }
        if (blob.size() - cursor < len) {
            if (out_error) *out_error = CodecError::TruncatedBody;
            return std::nullopt;
        }
        std::vector<std::uint8_t> body(blob.begin() + cursor, blob.begin() + cursor + len);
        cursor += len;
        auto [it, inserted] = out.emplace(field_id, std::move(body));
        if (!inserted) {
            if (out_error) *out_error = CodecError::DuplicateField;
            return std::nullopt;
        }
    }
    return out;
}

bool RunSelfTest() {
    // Byte-pinned fixture: matches save_codec.rs::tests::byte_pinned_two_fields.
    SaveFields fields;
    fields.emplace(0x0001, std::vector<std::uint8_t>{0x10, 0x20, 0x30, 0x40});
    fields.emplace(0x0042, std::vector<std::uint8_t>{0xCA, 0xFE});

    const std::array<std::uint8_t, 14> expected = {
        0x00, 0x01, 0x00, 0x04, 0x10, 0x20, 0x30, 0x40,
        0x00, 0x42, 0x00, 0x02, 0xCA, 0xFE,
    };

    CodecError err = CodecError::None;
    auto bytes = Encode(fields, &err);
    if (err != CodecError::None) return false;
    if (bytes.size() != expected.size()) return false;
    if (std::memcmp(bytes.data(), expected.data(), expected.size()) != 0) return false;

    // Empty blob round-trips to empty map.
    auto empty_decoded = Decode(std::span<const std::uint8_t>{}, &err);
    if (!empty_decoded || !empty_decoded->empty() || err != CodecError::None) return false;

    // Full round-trip.
    auto decoded = Decode(bytes, &err);
    if (!decoded || err != CodecError::None) return false;
    if (decoded->size() != fields.size()) return false;
    for (const auto& [id, data] : fields) {
        auto it = decoded->find(id);
        if (it == decoded->end() || it->second != data) return false;
    }

    // Truncated header rejected.
    std::array<std::uint8_t, 3> too_short = {0x00, 0x01, 0x00};
    if (Decode(too_short, &err).has_value()) return false;
    if (err != CodecError::TruncatedHeader) return false;

    // Truncated body rejected.
    std::array<std::uint8_t, 6> short_body = {0x00, 0x01, 0x00, 0x04, 0xAA, 0xBB};
    if (Decode(short_body, &err).has_value()) return false;
    if (err != CodecError::TruncatedBody) return false;

    // Duplicate field rejected.
    std::array<std::uint8_t, 10> dup = {
        0x00, 0x01, 0x00, 0x01, 0xAA,
        0x00, 0x01, 0x00, 0x01, 0xBB,
    };
    if (Decode(dup, &err).has_value()) return false;
    if (err != CodecError::DuplicateField) return false;

    return true;
}

}  // namespace dusk::net::save_codec
