#ifndef DUSK_NET_SAVE_SYNC_H
#define DUSK_NET_SAVE_SYNC_H

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace dusk::net::save_sync {

// Canonical-save field IDs inside the save_codec blob (see save_codec.h for
// the wire layout). M4 chunk 1 syncs the whole dSv_save_c block as one field;
// later chunks may split per-player consumables out into their own fields /
// the server-side player_profiles rows.
enum FieldId : std::uint16_t {
    kFieldSavedata = 1,  // raw bytes of dSv_info_c::mSavedata (dSv_save_c)
};

// Serialize the local game's canonical save (dSv_info_c::mSavedata) into a
// save_codec blob suitable for PUT /v1/sessions/{code}/save. Must be called
// on the sim thread (it reads g_dComIfG_gameInfo). Returns an empty vector if
// the save info isn't available yet.
std::vector<std::uint8_t> SerializeLocalSave();

// Decode `blob` and overwrite the local dSv_info_c::mSavedata with the
// canonical contents. Must be called on the sim thread at a safe point (the
// dusk::net::Tick() drain site is one). Returns false on a malformed blob,
// a missing/short kFieldSavedata field, or if the save info isn't ready.
bool ApplyCanonicalSave(std::span<const std::uint8_t> blob);

}  // namespace dusk::net::save_sync

#endif
