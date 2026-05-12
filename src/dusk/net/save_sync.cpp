#include "dusk/net/save_sync.h"

#include "dusk/logging.h"
#include "dusk/net/save_codec.h"

#include "d/d_com_inf_game.h"
#include "d/d_save.h"

#include <cstring>

namespace dusk::net::save_sync {

std::vector<std::uint8_t> SerializeLocalSave() {
    dSv_save_c* sd = dComIfGs_getSaveData();
    const auto* p = reinterpret_cast<const std::uint8_t*>(sd);

    save_codec::SaveFields fields;
    fields[kFieldSavedata] = std::vector<std::uint8_t>(p, p + sizeof(dSv_save_c));

    save_codec::CodecError err = save_codec::CodecError::None;
    auto blob = save_codec::Encode(fields, &err);
    if (err != save_codec::CodecError::None) {
        DuskLog.error("dusk::net::save_sync: encode failed (err={})",
                      static_cast<int>(err));
        return {};
    }
    return blob;
}

bool ApplyCanonicalSave(std::span<const std::uint8_t> blob) {
    save_codec::CodecError err = save_codec::CodecError::None;
    auto decoded = save_codec::Decode(blob, &err);
    if (!decoded) {
        DuskLog.error("dusk::net::save_sync: decode failed (err={})",
                      static_cast<int>(err));
        return false;
    }

    auto it = decoded->find(kFieldSavedata);
    if (it == decoded->end()) {
        DuskLog.error("dusk::net::save_sync: canonical blob has no savedata field");
        return false;
    }
    if (it->second.size() != sizeof(dSv_save_c)) {
        DuskLog.error("dusk::net::save_sync: savedata field is {} bytes, expected {}",
                      it->second.size(), sizeof(dSv_save_c));
        return false;
    }

    std::memcpy(dComIfGs_getSaveData(), it->second.data(), sizeof(dSv_save_c));
    DuskLog.debug("dusk::net::save_sync: applied canonical save ({} bytes)",
                  sizeof(dSv_save_c));
    return true;
}

}  // namespace dusk::net::save_sync
