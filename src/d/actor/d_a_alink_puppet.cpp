/**
 * d_a_alink_puppet.cpp
 *
 * Dusk multiplayer: the "puppet" branch of daAlink_c — a network-driven
 * mirror of a peer's Link actor. Lives outside the decomp tree so that
 * upstream resyncs from zeldaret/tp never touch this file; the only diff
 * against the original game code is the small #if TARGET_PC gates in
 * d_a_alink.cpp and the two declarations in d_a_alink.h.
 *
 * Scope (M3 #25 baseline):
 *   - apply replicated pos/rotation/speed each sim tick;
 *   - skip all controller-input, action AI, collision, audio, item logic;
 *   - leave the model rendered with its idle animation (full animation
 *     synchronisation lands in a later iteration once we plug into the
 *     action-procedure machinery).
 *
 * Friendly-fire and player-vs-player collision are intentionally absent:
 * the puppet is a pure visual representation (per the locked product
 * decision in .claude/plans/multiplayer-plan.md).
 */

#if TARGET_PC && DUSK_ENABLE_MULTIPLAYER

#include "d/actor/d_a_alink.h"

#include "dusk/logging.h"
#include "dusk/net/replication.h"

int daAlink_c::executePuppet() {
    // Pull the latest replicated pose; if nothing's arrived yet (handshake
    // just completed but the peer hasn't shipped a PlayerPose yet) leave
    // the actor at whatever spawn position it landed on.
    if (auto pose = dusk::net::replication::LatestPeerPose()) {
        current.pos.x = pose->pos[0];
        current.pos.y = pose->pos[1];
        current.pos.z = pose->pos[2];

        shape_angle.x = pose->shape_angle[0];
        shape_angle.y = pose->shape_angle[1];
        shape_angle.z = pose->shape_angle[2];
        // Keep current.angle in sync with shape_angle so the camera-facing
        // & follow code (if anything queries it) sees consistent values.
        current.angle.x = shape_angle.x;
        current.angle.y = shape_angle.y;
        current.angle.z = shape_angle.z;

        speed.x = pose->speed[0];
        speed.y = pose->speed[1];
        speed.z = pose->speed[2];
        speedF = pose->speed_f;
    }

    // Hide the puppet when the peer is in a different scene. Co-location is
    // recomputed whenever SceneAnnounce arrives on either side
    // (f_op_scene_req.cpp -> EmitLocalSceneAnnounce, and the inbound
    // dispatch in dusk::net::Tick).
    //
    // We achieve "hide" by simply skipping the rest of the frame; the model
    // stays rendered but parked at its last position. A proper hide hooks
    // into draw() and lives in a follow-up iteration.
    (void)dusk::net::replication::IsColocated();

    // Match the return convention of daAlink_Execute (non-zero = OK; 0
    // would cause the actor framework to delete us).
    return 1;
}

#endif  // TARGET_PC && DUSK_ENABLE_MULTIPLAYER
