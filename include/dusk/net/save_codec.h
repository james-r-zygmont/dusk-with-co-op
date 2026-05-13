#ifndef DUSK_NET_SAVE_CODEC_H
#define DUSK_NET_SAVE_CODEC_H

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace dusk::net::save_codec {

// Byte-identical port of tools/dusk-relay/src/save_codec.rs.
//
// Blob layout: back-to-back `(field_id u16 BE, len u16 BE, bytes)` tuples
// concatenated to EOF. Unknown field IDs are preserved verbatim on round-trip
// — that's the property that lets newer clients add fields without forcing
// a lock-step upgrade of older peers.
//
// `saves.schema_version` (column on the server) tracks codec version, not
// part of the blob itself.

inline constexpr std::uint16_t kSchemaVersion = 1;

// Sorted by field_id (std::map default ordering) — gives deterministic
// encode output to match Rust's BTreeMap iteration.
using SaveFields = std::map<std::uint16_t, std::vector<std::uint8_t>>;

enum class CodecError : std::uint8_t {
    None,
    TruncatedHeader,      // < 4 bytes left when a header was expected
    TruncatedBody,        // header says N bytes but fewer remain
    DuplicateField,       // same field_id appears twice in one blob
    FieldTooLong,         // body length > u16::max — encode side
};

// Encode always succeeds when every field body is <= 65535 bytes. On a
// too-long body the result is empty and `out_error` (if non-null) gets
// `FieldTooLong`.
std::vector<std::uint8_t> Encode(const SaveFields& fields, CodecError* out_error = nullptr);

// Returns nullopt on a malformed blob; the optional out_error pointer
// (if provided) is populated with the specific failure mode.
std::optional<SaveFields> Decode(std::span<const std::uint8_t> blob,
                                 CodecError* out_error = nullptr);

// Cross-language byte-pin: mirrors save_codec.rs::tests::byte_pinned_two_fields.
// Logged on net::Init() so drift between client and server surfaces loudly.
bool RunSelfTest();

}  // namespace dusk::net::save_codec

#endif
