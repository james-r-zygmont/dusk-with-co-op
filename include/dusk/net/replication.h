#ifndef DUSK_NET_REPLICATION_H
#define DUSK_NET_REPLICATION_H

#include "dusk/net/wire.h"

#include <cstdint>
#include <optional>

class fopAc_ac_c;

namespace dusk::net::replication {

// True if the given fpc_ProcID was spawned via RequestPuppetSpawn. We use
// a side-table rather than a bit in the parameters u32 because the stage
// data spawns the local Link with parameters = -1 (all bits set), making
// any bit-based puppet flag give false positives on the local Link.
bool IsPuppetId(fpc_ProcID id);

// True if this actor's ProcID was registered as a puppet. Free function
// (rather than a method on daAlink_c) so non-daAlink callers can use it
// too.
bool IsPuppet(const fopAc_ac_c* actor);

// Outbound: read the local Link's transform fields and emit a PlayerPose
// packet to the wire. Called from the M3 outbound replication path (#27).
// Returns true if a packet was sent. Send-on-delta gating lives here.
bool BroadcastLocalPose();

// Outbound: read the current stage name + room number and emit a
// SceneAnnounce packet. Hooked from f_op_scene_req.cpp's Done phase
// (#26) so the peer learns about scene transitions promptly. Also
// updates the local-scene cache used by IsColocated().
bool EmitLocalSceneAnnounce();

// Outbound: record the body animation the local Link just switched to.
// Hooked from daAlink_c::setSingleAnime / setDoubleAnime (the two body-anim
// entry points). Cheap — just stores the id; BroadcastLocalAnim() ships it.
void NoteLocalAnim(int anmId);

// Outbound: emit a PlayerAnim packet if the local Link's animation changed
// since the last send. Called from Tick() after BroadcastLocalPose().
bool BroadcastLocalAnim();

// Inbound: stash the latest pose snapshot received from the peer. The
// puppet's executePuppet() (in d_a_alink_puppet.cpp, landing in #25) reads
// the latest snapshot each sim tick and applies it to its own transform.
void OnPeerPose(const wire::PlayerPose& pose);
void OnPeerAnim(const wire::PlayerAnim& anim);
void OnPeerSceneAnnounce(const wire::SceneAnnounce& announce);

// The puppet implementation queries these from inside executePuppet().
// Returns nullopt if no replicated value has arrived yet.
std::optional<wire::PlayerPose> LatestPeerPose();
std::optional<wire::PlayerAnim> LatestPeerAnim();

// True when both clients have reported the same stage + room. Updated every
// tick from the PlayerPose stream (which carries stage/room) plus the
// SceneAnnounce hint. Shown in the debug HUD.
bool IsColocated();

// True only when we *positively* know the peer is in a different stage/room
// (both scenes known and they differ). Returns false when the peer's scene
// is still unknown — so the puppet's draw gate fails open (renders) rather
// than hiding a puppet we just haven't placed yet.
bool IsPeerInDifferentScene();

// Request that a puppet daAlink_c be spawned on the next sim tick when a
// scene is loaded. Safe to call from anywhere on the sim thread; the
// actual fopAcM_create happens inside Tick() so we never spawn on the
// network worker thread or mid-scene-load.
void RequestPuppetSpawn();

// Register `id` as the puppet's ProcID. Called from daAlink_Create() when
// it detects a puppet (a daAlink_c created while a different Link is already
// the local player). Used by the despawn machinery to find the puppet;
// daAlink_c::isPuppet() also derives the answer independently.
void RegisterPuppetId(fpc_ProcID id);

// Despawn the puppet if it exists. Called on Disconnect.
void DespawnPuppet();

// True if a puppet actor currently exists.
bool PuppetExists();

// Called by dusk::net::Tick() each sim tick to advance any per-frame
// replication bookkeeping (send-on-delta cadence, interp buffer GC, etc.).
void Tick();

// Diagnostics for the ImGui Co-op Debug HUD.
struct DebugPuppetInfo {
    bool puppetExists = false;
    fpc_ProcID puppetId = 0xFFFFFFFFu;
    bool puppetRecognized = false;   // IsPuppet(puppet actor) — should be true
    float puppetPos[3] = {0.f, 0.f, 0.f};
    bool hasPeerPose = false;
    std::uint32_t peerPoseTick = 0;
    float peerPos[3] = {0.f, 0.f, 0.f};
    bool localLinkExists = false;
    bool localLinkRecognizedAsPuppet = false;  // IsPuppet(local Link) — should be false!
    bool colocated = false;
};
DebugPuppetInfo GetDebugPuppetInfo();

}  // namespace dusk::net::replication

#endif
