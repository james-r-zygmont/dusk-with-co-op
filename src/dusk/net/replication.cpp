#include "dusk/net/replication.h"

#include "dusk/logging.h"
#include "dusk/net/net.h"

#include "d/actor/d_a_player.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor.h"
#include "f_op/f_op_actor_mng.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>

namespace dusk::net::replication {

namespace {

// Worker thread (network) writes the peer-side caches; sim thread (Tick /
// puppet execute) reads. std::mutex is fine — these are at-most-30Hz events.
std::mutex g_mutex;
std::optional<wire::PlayerPose> g_latestPose;
std::optional<wire::PlayerAnim> g_latestAnim;
std::optional<wire::SceneAnnounce> g_peerScene;
std::optional<wire::SceneAnnounce> g_localScene;
bool g_colocated = false;

// Outbound bookkeeping — touched only on the sim thread.
std::uint32_t g_outboundTick = 0;
std::optional<wire::PlayerPose> g_lastSentPose;

// Puppet-spawn bookkeeping — touched only on the sim thread.
bool g_wantPuppet = false;
fpc_ProcID g_puppetId = fpcM_ERROR_PROCESS_ID_e;

// Thresholds for the send-on-delta gate. Picked generously — the network
// budget is trivial at our 30 Hz / 48 B per pose, and false positives just
// mean an extra packet. Adjust during M7 polish if bandwidth becomes a
// concern.
constexpr float kPosEpsilon = 0.05f;       // world units (~5cm in TP scale)
constexpr float kSpeedEpsilon = 0.05f;
constexpr std::int16_t kAngleEpsilon = 16; // s16 angle units (~0.09°)
// Always send at least this often even if pose hasn't changed; serves both
// as a heartbeat and a recovery point after a UDP-equivalent gap.
constexpr std::uint32_t kHeartbeatTicks = 30;  // ~1s at 30Hz

void RecomputeColocation() {
    // Caller must hold g_mutex.
    if (!g_peerScene || !g_localScene) {
        g_colocated = false;
        return;
    }
    g_colocated = g_peerScene->stage == g_localScene->stage
               && g_peerScene->room == g_localScene->room;
}

bool ApproxEqualPose(const wire::PlayerPose& a, const wire::PlayerPose& b) {
    if (a.stage != b.stage) return false;
    if (a.room != b.room) return false;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(a.pos[i] - b.pos[i]) > kPosEpsilon) return false;
        if (std::fabs(a.speed[i] - b.speed[i]) > kSpeedEpsilon) return false;
        const auto diff = static_cast<std::int32_t>(a.shape_angle[i])
                        - static_cast<std::int32_t>(b.shape_angle[i]);
        if (std::abs(diff) > kAngleEpsilon) return false;
    }
    if (std::fabs(a.speed_f - b.speed_f) > kSpeedEpsilon) return false;
    return true;
}

void FillStage(std::array<std::uint8_t, wire::kStageNameLen>& out, const char* name) {
    out.fill(0);
    if (name == nullptr) return;
    const std::size_t n = std::min(std::strlen(name), wire::kStageNameLen);
    std::memcpy(out.data(), name, n);
}

}  // namespace

bool IsPuppet(const fopAc_ac_c* actor) {
    if (actor == nullptr) return false;
    return (fopAcM_GetParam(actor) & kPuppetParamBit) != 0u;
}

bool BroadcastLocalPose() {
    // Resolve the local player. dComIfGp_getLinkPlayer() returns daPy_py_c*
    // which is the player base; we only read fopAc_ac_c members below.
    daPy_py_c* link = dComIfGp_getLinkPlayer();
    if (link == nullptr) return false;

    wire::PlayerPose pose;
    pose.tick = ++g_outboundTick;
    FillStage(pose.stage, dComIfGp_getStartStageName());
    pose.room = static_cast<std::uint8_t>(link->current.roomNo);
    pose.pos[0] = link->current.pos.x;
    pose.pos[1] = link->current.pos.y;
    pose.pos[2] = link->current.pos.z;
    pose.shape_angle[0] = link->shape_angle.x;
    pose.shape_angle[1] = link->shape_angle.y;
    pose.shape_angle[2] = link->shape_angle.z;
    pose.speed[0] = link->speed.x;
    pose.speed[1] = link->speed.y;
    pose.speed[2] = link->speed.z;
    pose.speed_f = link->speedF;

    // Track the local stage/room for co-location detection. SceneAnnounce
    // emit hook lives in #26 (f_op_scene_req Done) but caching here lets
    // IsColocated() converge as soon as both clients are running.
    {
        wire::SceneAnnounce announce;
        announce.stage = pose.stage;
        announce.room = pose.room;
        announce.spawn = 0;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_localScene || g_localScene->stage != announce.stage
            || g_localScene->room != announce.room)
        {
            g_localScene = announce;
            RecomputeColocation();
        }
    }

    // Send-on-delta: skip if pose is essentially unchanged AND we sent
    // recently. The heartbeat path guarantees at-least 1Hz traffic so the
    // peer's interp buffer doesn't go stale.
    const bool heartbeat_due =
        !g_lastSentPose
        || (g_outboundTick - g_lastSentPose->tick) >= kHeartbeatTicks;
    if (g_lastSentPose && !heartbeat_due
        && ApproxEqualPose(*g_lastSentPose, pose))
    {
        return false;
    }

    auto bytes = wire::Encode(wire::Frame{pose});
    if (Send(bytes)) {
        g_lastSentPose = pose;
        return true;
    }
    return false;
}

bool EmitLocalSceneAnnounce() {
    wire::SceneAnnounce announce;
    FillStage(announce.stage, dComIfGp_getStartStageName());
    announce.room = static_cast<std::uint8_t>(dComIfGp_roomControl_getStayNo());
    announce.spawn = 0;

    // Update the local cache + recompute co-location regardless of connection
    // state — this lets IsColocated() stay sane even when offline.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_localScene = announce;
        RecomputeColocation();
    }

    if (!IsConnected() || GetStats().playerIndex == 0xFF) {
        return false;
    }
    auto bytes = wire::Encode(wire::Frame{announce});
    return Send(bytes);
}

void OnPeerPose(const wire::PlayerPose& pose) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_latestPose = pose;
}

void OnPeerAnim(const wire::PlayerAnim& anim) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_latestAnim = anim;
}

void OnPeerSceneAnnounce(const wire::SceneAnnounce& announce) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_peerScene = announce;
    RecomputeColocation();
    DuskLog.debug("dusk::net::replication: peer scene = {}/{} (colocated={})",
                  reinterpret_cast<const char*>(announce.stage.data()),
                  announce.room, g_colocated);
}

std::optional<wire::PlayerPose> LatestPeerPose() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latestPose;
}

std::optional<wire::PlayerAnim> LatestPeerAnim() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latestAnim;
}

bool IsColocated() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_colocated;
}

void RequestPuppetSpawn() {
    g_wantPuppet = true;
}

bool PuppetExists() {
    return g_puppetId != fpcM_ERROR_PROCESS_ID_e
        && fopAcM_SearchByID(g_puppetId) != nullptr;
}

void DespawnPuppet() {
    g_wantPuppet = false;
    if (g_puppetId != fpcM_ERROR_PROCESS_ID_e) {
        if (auto* puppet = fopAcM_SearchByID(g_puppetId)) {
            fopAcM_delete(puppet);
        }
        g_puppetId = fpcM_ERROR_PROCESS_ID_e;
    }
}

namespace {

void MaybeSpawnPuppet() {
    if (!g_wantPuppet) return;
    if (PuppetExists()) {
        g_wantPuppet = false;
        return;
    }
    // We need the local player loaded to spawn next to them; if Link isn't
    // ready yet (still loading the scene), defer to the next tick.
    daPy_py_c* link = dComIfGp_getLinkPlayer();
    if (link == nullptr) return;

    cXyz pos = link->current.pos;
    csXyz angle = link->shape_angle;
    const u32 parameters = kPuppetParamBit;

    g_puppetId = fopAcM_create(
        fpcNm_ALINK_e, parameters, &pos, fopAcM_GetRoomNo(link),
        &angle, /*scale=*/nullptr, /*argument=*/-1);
    if (g_puppetId == fpcM_ERROR_PROCESS_ID_e) {
        DuskLog.warn("dusk::net::replication: puppet spawn failed; will retry");
        return;
    }
    DuskLog.debug("dusk::net::replication: puppet spawned id={}", g_puppetId);
    g_wantPuppet = false;
}

}  // namespace

void Tick() {
    MaybeSpawnPuppet();

    // Gate outbound replication on a complete handshake. Stats.playerIndex
    // stays at 0xFF until HelloAck has been processed; sending poses before
    // then is wasted bandwidth and the relay won't route them (the WS
    // handshake hasn't authorised the slot yet).
    if (!IsConnected()) return;
    if (GetStats().playerIndex == 0xFF) return;
    BroadcastLocalPose();
}

}  // namespace dusk::net::replication
