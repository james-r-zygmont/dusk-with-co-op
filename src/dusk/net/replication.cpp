#include "dusk/net/replication.h"

#include "dusk/logging.h"
#include "dusk/net/coop_log.h"
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
// -1 = nothing recorded yet. g_localAnimId is updated by NoteLocalAnim()
// from inside daAlink_c's anim setters; g_lastSentAnimId tracks what we've
// already shipped so BroadcastLocalAnim() only sends on change. The tick
// stamp lets us re-announce on a slow heartbeat (PlayerAnim is otherwise
// purely event-driven, so a late-joining peer or a dropped packet would
// leave the puppet stuck until the next animation change).
int g_localAnimId = -1;
int g_lastSentAnimId = -1;
std::uint32_t g_lastSentAnimTick = 0;

// Puppet-spawn bookkeeping — touched only on the sim thread. g_puppetId is
// the puppet's ProcID once known; it's set both from fopAcM_create's return
// value (a fallback) and authoritatively by RegisterPuppetId() when
// daAlink_Create() detects the puppet. daAlink_c::isPuppet() doesn't depend
// on g_puppetId being correct — it has an independent derivation — so the
// id is used only by DespawnPuppet/PuppetExists.
bool g_wantPuppet = false;
fpc_ProcID g_puppetId = fpcM_ERROR_PROCESS_ID_e;

// "Don't (re-)spawn the puppet yet" countdown (sim-thread frames). The puppet
// is a full daAlink_c — heavy enough that a cutscene's demo-data parse can
// fail with it around (it did: d_demo.cpp's "デモデータ読み込みエラー" then a
// crash). So we keep it despawned while a stage loads, during cutscenes, and
// for a short grace period after. OnSceneTransition arms it big (covers the
// scene load); EmitLocalSceneAnnounce (the Done phase) caps it to the post-
// load grace; MaybeSpawnPuppet/Tick re-arm it to the event grace whenever a
// cutscene is running.
// Generous on purpose: a long multi-scene cutscene (demo09 = Faron Light
// Spirit, stages F_SP102→103→104→…) briefly reads !event_runCheck() between
// its sub-states — a ~20s lull on F_SP104 was enough for the old 20s grace to
// expire, the puppet to re-spawn, and the next demo's data alloc to come back
// NULL ("デモデータ読み込みエラー", confirmed both instances). 60s covers
// those lulls with margin (and the cutscene's frequent scene transitions keep
// re-arming it anyway). Cost: the puppet reappears slowly after events / scene
// loads. Crude band-aid; the real fix is a leaner puppet create() that doesn't
// allocate the full anim-heap battery so it can coexist in a tight scene.
constexpr int kPuppetEventGraceFrames = 60 * 30;  // ~60s after a cutscene / scene Done
constexpr int kPuppetSceneLoadFrames = 60 * 30;   // safety cap if Done never fires
int g_puppetSettleFrames = 0;
bool g_eventWasRunning = false;                    // rising-edge tracking for despawn-on-event

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

bool IsPuppetId(fpc_ProcID id) {
    // For two-player co-op there's only ever one puppet so an integer
    // comparison is enough; if we ever support N players this becomes a
    // std::unordered_set lookup.
    return id != fpcM_ERROR_PROCESS_ID_e && id == g_puppetId;
}

bool IsPuppet(const fopAc_ac_c* actor) {
    if (actor == nullptr) return false;
    return IsPuppetId(fopAcM_GetID(actor));
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

void NoteLocalAnim(int anmId) {
    // Sim-thread only. The Tick() pump diffs against g_lastSentAnimId.
    if (anmId != g_localAnimId) {
        COOP_LOG("anim local -> {}", anmId);
    }
    g_localAnimId = anmId;
}

bool BroadcastLocalAnim() {
    if (g_localAnimId < 0) return false;
    const bool heartbeat_due =
        g_lastSentAnimId < 0 || (g_outboundTick - g_lastSentAnimTick) >= kHeartbeatTicks;
    if (!heartbeat_due && g_localAnimId == g_lastSentAnimId) return false;

    wire::PlayerAnim anim;
    anim.anim_id = static_cast<std::uint16_t>(g_localAnimId);
    // Receiver replays from the animation's natural start frame (the puppet
    // re-runs setSingleAnimeBase, which seeds the frame the same way), so 0
    // is consistent with how the host's setSingleAnime call started it. Held
    // item / wolf form aren't replicated yet — see the M3 follow-up notes.
    anim.frame = 0.f;
    anim.transform = wire::kTransformHuman;
    anim.held_item = wire::kHeldItemNone;

    auto bytes = wire::Encode(wire::Frame{anim});
    if (Send(bytes)) {
        g_lastSentAnimId = g_localAnimId;
        g_lastSentAnimTick = g_outboundTick;
        return true;
    }
    return false;
}

bool EmitLocalSceneAnnounce() {
    wire::SceneAnnounce announce;
    FillStage(announce.stage, dComIfGp_getStartStageName());
    announce.room = static_cast<std::uint8_t>(dComIfGp_roomControl_getStayNo());
    announce.spawn = 0;

    // The new scene has finished loading (this is the f_op_scene_req Done
    // phase) — shorten the "don't re-spawn the puppet yet" window from the big
    // scene-load value down to the post-load grace. If an entrance cutscene is
    // about to play, Tick()/MaybeSpawnPuppet will re-arm it.
    if (g_puppetSettleFrames > kPuppetEventGraceFrames) {
        g_puppetSettleFrames = kPuppetEventGraceFrames;
    }

    // Update the local cache + recompute co-location regardless of connection
    // state — this lets IsColocated() stay sane even when offline.
    bool colocated;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_localScene = announce;
        RecomputeColocation();
        colocated = g_colocated;
    }
    COOP_LOG("scene done = {}/{} (colocated={}, puppet settle {} frames)",
             reinterpret_cast<const char*>(announce.stage.data()), announce.room,
             colocated, g_puppetSettleFrames);

    if (!IsConnected() || GetStats().playerIndex == 0xFF) {
        return false;
    }
    auto bytes = wire::Encode(wire::Frame{announce});
    return Send(bytes);
}

void OnPeerPose(const wire::PlayerPose& pose) {
    // A peer pose means a peer is present and active — keep wanting a puppet
    // (MaybeSpawnPuppet re-spawns it after every scene transition, since the
    // puppet is a scene actor and dies with the scene). Cleared on Disconnect /
    // the debug HUD's Despawn. g_wantPuppet is sim-thread-only and OnPeerPose
    // runs on the sim thread (net::Tick's inbound drain).
    g_wantPuppet = true;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_latestPose = pose;
    // The pose stream carries stage/room every tick, so it's a far more
    // reliable co-location source than SceneAnnounce (which only fires on a
    // scene transition — a pair that connects while already in the same scene
    // would otherwise never see each other). Treat the latest pose as the
    // peer's authoritative scene.
    if (!g_peerScene || g_peerScene->stage != pose.stage || g_peerScene->room != pose.room) {
        wire::SceneAnnounce s;
        s.stage = pose.stage;
        s.room = pose.room;
        s.spawn = 0;
        g_peerScene = s;
        RecomputeColocation();
    }
}

void OnPeerAnim(const wire::PlayerAnim& anim) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool changed = !g_latestAnim || g_latestAnim->anim_id != anim.anim_id;
    g_latestAnim = anim;
    if (changed) {
        COOP_LOG("anim peer -> {}", anim.anim_id);
    }
}

void OnPeerSceneAnnounce(const wire::SceneAnnounce& announce) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_peerScene = announce;
    RecomputeColocation();
    COOP_LOG("scene peer = {}/{} (colocated={})",
             reinterpret_cast<const char*>(announce.stage.data()), announce.room, g_colocated);
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

bool IsPeerInDifferentScene() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_peerScene || !g_localScene) return false;  // unknown -> fail open
    return g_peerScene->stage != g_localScene->stage
        || g_peerScene->room != g_localScene->room;
}

void RequestPuppetSpawn() {
    g_wantPuppet = true;
}

void OnSceneTransition() {
    // A stage change is starting. The puppet (an old-scene actor) gets deleted
    // by the scene teardown — the *safe* place to delete it (the real Link is
    // going away too), so we don't fopAcM_delete it here (mid-scene deletes
    // have crashed the host). We just block it from *re-spawning* until the new
    // scene has loaded and settled — a stray daAlink_c around during a
    // cutscene's demo-data parse has derailed cutscenes. EmitLocalSceneAnnounce
    // (Done) trims this back to the post-load grace.
    g_puppetSettleFrames = kPuppetSceneLoadFrames;
    COOP_LOG("scene transition (going to {}) — puppet held {} frames",
             dComIfGp_getStartStageName(), kPuppetSceneLoadFrames);
}

void RegisterPuppetId(fpc_ProcID id) {
    g_puppetId = id;
    COOP_LOG("puppet id registered = {}", id);
}

bool PuppetExists() {
    if (g_puppetId == fpcM_ERROR_PROCESS_ID_e) return false;
    if (fopAcM_SearchByID(g_puppetId) != nullptr) return true;
    // The actor framework has collected the puppet; only now is it safe to
    // forget the ProcID. Crucially we do NOT clear g_puppetId in
    // DespawnPuppet() — fopAcM_delete only flags the actor, and ~daAlink_c()
    // runs on a later delete pass; isPuppet() must still resolve true inside
    // that destructor or it will clobber the local player's state.
    g_puppetId = fpcM_ERROR_PROCESS_ID_e;
    return false;
}

void DespawnPuppet() {
    g_wantPuppet = false;
    if (g_puppetId != fpcM_ERROR_PROCESS_ID_e) {
        COOP_LOG("puppet despawn id={}", g_puppetId);
        if (auto* puppet = fopAcM_SearchByID(g_puppetId)) {
            fopAcM_delete(puppet);
        }
        // g_puppetId stays set until PuppetExists() confirms the actor is
        // gone — see the comment there.
    }
}

namespace {

void MaybeSpawnPuppet() {
    if (!g_wantPuppet) return;
    // Already spawned — nothing to do. We deliberately keep g_wantPuppet set so
    // that when the puppet dies with a scene transition we re-spawn it in the
    // new scene next time Link is ready.
    if (PuppetExists()) return;
    // Don't re-spawn while a scene is loading, a cutscene is running, or during
    // the grace period after either — see g_puppetSettleFrames (managed in
    // Tick() / OnSceneTransition() / EmitLocalSceneAnnounce()).
    if (g_puppetSettleFrames > 0) return;
    // We need the local player loaded to spawn next to them; if Link isn't
    // ready yet (still loading the scene), defer to the next tick.
    daPy_py_c* link = dComIfGp_getLinkPlayer();
    if (link == nullptr) return;

    cXyz pos = link->current.pos;
    csXyz angle = link->shape_angle;
    // Parameters = 0 — the puppet's "I'm a puppet" identity is derived in
    // daAlink_c::isPuppet() (a Link that isn't the registered local player),
    // not from anything in this u32.
    const u32 parameters = 0;

    const fpc_ProcID id = fopAcM_create(
        fpcNm_ALINK_e, parameters, &pos, fopAcM_GetRoomNo(link),
        &angle, /*scale=*/nullptr, /*argument=*/-1);
    if (id == fpcM_ERROR_PROCESS_ID_e) {
        DuskLog.warn("[coop] puppet spawn failed (fopAcM_create); will retry");
        return;
    }
    // Fallback: daAlink_Create() also calls RegisterPuppetId with the actor's
    // own ProcID, which is authoritative. This handles the case where the
    // create() detection somehow missed (it shouldn't).
    if (g_puppetId == fpcM_ERROR_PROCESS_ID_e) {
        g_puppetId = id;
    }
    COOP_LOG("puppet spawn requested id={} at ({:.0f},{:.0f},{:.0f})",
             id, pos.x, pos.y, pos.z);
}

}  // namespace

DebugPuppetInfo GetDebugPuppetInfo() {
    DebugPuppetInfo info;
    info.puppetId = g_puppetId;
    if (g_puppetId != fpcM_ERROR_PROCESS_ID_e) {
        if (const fopAc_ac_c* puppet = fopAcM_SearchByID(g_puppetId)) {
            info.puppetExists = true;
            info.puppetRecognized = IsPuppet(puppet);
            info.puppetPos[0] = puppet->current.pos.x;
            info.puppetPos[1] = puppet->current.pos.y;
            info.puppetPos[2] = puppet->current.pos.z;
        }
    }
    if (const fopAc_ac_c* link = dComIfGp_getLinkPlayer()) {
        info.localLinkExists = true;
        info.localLinkRecognizedAsPuppet = IsPuppet(link);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_latestPose) {
            info.hasPeerPose = true;
            info.peerPoseTick = g_latestPose->tick;
            info.peerPos[0] = g_latestPose->pos[0];
            info.peerPos[1] = g_latestPose->pos[1];
            info.peerPos[2] = g_latestPose->pos[2];
        }
        info.colocated = g_colocated;
    }
    return info;
}

void Tick() {
    // GC the puppet ProcID as soon as the actor framework has collected it,
    // even while disconnected (MaybeSpawnPuppet only does this while it wants a
    // puppet) — keeps a stale g_puppetId from ever colliding with a recycled
    // ProcID and making isPuppet() misfire on a real Link.
    (void)PuppetExists();

    // While an event/cutscene is running, hold the puppet OFF (re-arm the
    // grace) — but do NOT actively despawn it here. Deleting the puppet
    // mid-scene (while the real Link survives) crashes the host: an NPC talk
    // event triggered DespawnPuppet() and the client died a couple frames
    // later. The puppet only gets deleted safely at scene teardown (the real
    // Link is going away too) or on Disconnect; so we just stop it from
    // *re-spawning* during/right-after an event and let it ride.
    const bool eventRunning = dComIfGp_event_runCheck();
    if (eventRunning) {
        if (!g_eventWasRunning) COOP_LOG("event started — puppet held {} frames",
                                         kPuppetEventGraceFrames);
        g_puppetSettleFrames = kPuppetEventGraceFrames;
    } else {
        if (g_eventWasRunning) COOP_LOG("event ended");
        if (g_puppetSettleFrames > 0) --g_puppetSettleFrames;
    }
    g_eventWasRunning = eventRunning;

    MaybeSpawnPuppet();

    // Gate outbound replication on a complete handshake. Stats.playerIndex
    // stays at 0xFF until HelloAck has been processed; sending poses before
    // then is wasted bandwidth and the relay won't route them (the WS
    // handshake hasn't authorised the slot yet).
    if (!IsConnected()) return;
    if (GetStats().playerIndex == 0xFF) return;
    BroadcastLocalPose();
    BroadcastLocalAnim();
}

}  // namespace dusk::net::replication
