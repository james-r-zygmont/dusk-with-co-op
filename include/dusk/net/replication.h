#ifndef DUSK_NET_REPLICATION_H
#define DUSK_NET_REPLICATION_H

#include "dusk/net/wire.h"

#include <cstdint>
#include <optional>

class fopAc_ac_c;

namespace dusk::net::replication {

// The high bit of the `parameters` u32 passed to `fopAcM_create` flags this
// actor as a network-driven puppet. Inspected via `fopAcM_GetParam` — see
// `include/f_op/f_op_actor_mng.h:184`.
//
// We picked the high bit because daAlink_c uses only the low bits of the
// parameters field (room number, mode, etc. — see daPy_py_c::setParamData
// in d_a_player.cpp). High bit being set on a daAlink_c spawn is therefore
// unambiguously a Dusk-only signal.
inline constexpr std::uint32_t kPuppetParamBit = 0x80000000u;

// True if this actor was spawned with kPuppetParamBit set.
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

// True when both clients have reported the same stage + room via
// SceneAnnounce. Drives the puppet's draw gate (hide when split, render
// when co-located) per plan §C.
bool IsColocated();

// Request that a puppet daAlink_c be spawned on the next sim tick when a
// scene is loaded. Safe to call from anywhere on the sim thread; the
// actual fopAcM_create happens inside Tick() so we never spawn on the
// network worker thread or mid-scene-load.
void RequestPuppetSpawn();

// Despawn the puppet if it exists. Called on Disconnect.
void DespawnPuppet();

// True if a puppet actor currently exists.
bool PuppetExists();

// Called by dusk::net::Tick() each sim tick to advance any per-frame
// replication bookkeeping (send-on-delta cadence, interp buffer GC, etc.).
void Tick();

}  // namespace dusk::net::replication

#endif
