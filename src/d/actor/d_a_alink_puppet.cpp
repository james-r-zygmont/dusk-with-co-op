/**
 * d_a_alink_puppet.cpp
 *
 * Dusk multiplayer: the "puppet" branch of daAlink_c — a network-driven
 * mirror of a peer's Link actor. Lives outside the decomp tree so that
 * upstream resyncs from zeldaret/tp never touch this file; the only diff
 * against the original game code is the small #if TARGET_PC gates in
 * d_a_alink.cpp and the two declarations in d_a_alink.h.
 *
 * Scope:
 *   - apply replicated pos/rotation/speed each sim tick;
 *   - apply the replicated body animation (PlayerAnim, sent on change) by
 *     re-running setSingleAnimeBase and letting allAnimePlay() advance the
 *     frame controllers locally — so walk/run/idle/jump/roll/sword read
 *     correctly without streaming per-frame anim state;
 *   - skip all controller-input, action AI, collision, audio, item logic.
 *
 * Not yet replicated (M3 follow-ups): blended walk↔run cross-fade (we pop at
 * the 0.5 weight crossover), per-clip anim speed, re-trigger of the *same*
 * clip (e.g. a second sword swing — needs a restart counter on the wire),
 * held-item models, wolf form, and face texture (blink/mouth) animation.
 *
 * Note on the shared model data: a second daAlink_c (us) and the real Link
 * share the "Alink" J3DModelData, and daAlink_c::changeModelDataDirect() binds
 * joints 0/1/16's J3DMtxCalc on that *shared* data to the calling actor's blend
 * tables — so whichever Link bound them last drives the other's skeleton too.
 * executePuppet() works around it by re-binding to our blend tables before our
 * own model calc, then handing the binding back to the real Link (whose
 * execute() evaluates its skeleton separately in the same frame). A proper fix
 * is the leaner puppet create() that doesn't touch the shared data at all.
 *
 * Friendly-fire and player-vs-player collision are intentionally absent:
 * the puppet is a pure visual representation (per the locked product
 * decision in .claude/plans/multiplayer-plan.md).
 */

#if TARGET_PC && DUSK_ENABLE_MULTIPLAYER

#include "d/actor/d_a_alink.h"

#include "dusk/logging.h"
#include "dusk/net/coop_log.h"
#include "dusk/net/replication.h"

#include "d/d_com_inf_game.h"
#include "d/d_kankyo.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_mtx.h"

bool daAlink_c::isPuppet() const {
    // Fast path: the despawn machinery registers the puppet's ProcID.
    if (dusk::net::replication::IsPuppetId(fopAcM_GetID(this))) return true;

    // Robust path: the game has exactly one local Link, registered via
    // dComIfGp_setLinkPlayer. If a *different* Link is registered as the
    // local player, this daAlink_c can only be a Dusk co-op puppet. This
    // holds from the first line of the puppet's create() — the local Link
    // registered itself long before the puppet is ever spawned — and it
    // stays correct across scene transitions because the outgoing Link's
    // destructor clears the slot before the incoming Link's create() runs.
    const daPy_py_c* localLink = dComIfGp_getLinkPlayer();
    return localLink != nullptr && localLink != this;
}

int daAlink_c::executePuppet() {
    // Rate-limited heartbeat so the log shows executePuppet is actually
    // running and what pose it's applying.
    static int s_logTick = 0;
    const bool logThisTick = (++s_logTick % 120) == 0;

    // Pull the latest replicated pose; if nothing's arrived yet (handshake
    // just completed but the peer hasn't shipped a PlayerPose yet) leave
    // the actor at whatever spawn position it landed on.
    auto poseOpt = dusk::net::replication::LatestPeerPose();
    auto animOpt = dusk::net::replication::LatestPeerAnim();
    if (logThisTick) {
        if (poseOpt) {
            COOP_LOG("puppet exec: this={} pose tick={} pos=({:.1f},{:.1f},{:.1f}) anm={}",
                     (const void*)this, poseOpt->tick,
                     poseOpt->pos[0], poseOpt->pos[1], poseOpt->pos[2],
                     animOpt ? (int)animOpt->anim_id : -1);
        } else {
            COOP_LOG("puppet exec: this={} (no peer pose yet)", (const void*)this);
        }
    }
    if (auto& pose = poseOpt) {
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

    // Rebuild the model matrices and advance the animation from the replicated
    // state. The normal daAlink_c::execute() path does this every frame (the
    // mDoMtx_stack_c::transS / ZXYrotM / setBaseTRMtx sequence, then
    // allAnimePlay() to step the frame controllers, then mpLinkModel->calc(),
    // then the sub-model parenting); executePuppet() short-circuits execute(),
    // so we do the minimal version here or the model — and its face/hat/hand
    // sub-models — stay parked in the bind pose.
    //
    // The peer sends PlayerAnim only when its body animation *changes* (plus a
    // ~1 Hz heartbeat), so we re-run setSingleAnimeBase on each new id and let
    // allAnimePlay() carry the clip forward locally. setSingleAnimeBase uses
    // the default speed/morf — per-clip speed isn't on the wire yet — which is
    // close enough visually. The id is range-checked because it arrives off
    // the network and getAnmData()/getMainBckData() index fixed-size tables.
    if (mpLinkModel != NULL) {
        // Re-bind the shared Alink model data's joint matrix calculators to
        // our blend tables before we evaluate (the real Link's create() or a
        // clothes change may have bound them to its tables). See the file
        // header. changeModelDataDirect(0) does the rebinding without
        // re-entering the face texture animators.
        changeModelDataDirect(0);

        static int s_appliedAnimId = -1;
        if (auto& anim = animOpt) {
            const int id = anim->anim_id;
            if (id != s_appliedAnimId && id >= 0 && id < ANM_MAX) {
                setSingleAnimeBase((daAlink_ANM)id);
                s_appliedAnimId = id;
            }
        }

        mDoMtx_stack_c::transS(current.pos.x, current.pos.y, current.pos.z);
        mDoMtx_stack_c::ZXYrotM(shape_angle.x, shape_angle.y, shape_angle.z);
        mpLinkModel->setBaseTRMtx(mDoMtx_stack_c::get());
        allAnimePlay();
        mpLinkModel->calc();

        // Face + hat parent to the body's head joint (4); hands share the
        // body base matrix with the wrist joints (9, 0xE) overridden — joint
        // indices cribbed from d_a_alink.cpp's normal model-update path.
        if (mpLinkFaceModel != NULL) {
            mpLinkFaceModel->setBaseTRMtx(mpLinkModel->getAnmMtx(4));
            mpLinkFaceModel->calc();
        }
        if (mpLinkHatModel != NULL) {
            mpLinkHatModel->setBaseTRMtx(mpLinkModel->getAnmMtx(4));
            mpLinkHatModel->calc();
        }
        if (mpLinkHandModel != NULL) {
            mpLinkHandModel->setBaseTRMtx(mpLinkModel->getBaseTRMtx());
            mpLinkHandModel->calc();
            mpLinkHandModel->setAnmMtx(1, mpLinkModel->getAnmMtx(9));
            mpLinkHandModel->setAnmMtx(2, mpLinkModel->getAnmMtx(0xE));
        }

        // Hand the shared joint bindings back to the real Link so its own
        // execute()/calc() (which runs separately this same frame) drives its
        // own animation rather than ours. Only when it's actually using the
        // same (human) model data and isn't mid-clothes-change.
        daAlink_c* link = (daAlink_c*)dComIfGp_getLinkPlayer();
        if (link != NULL && link != this && link->mClothesChangeWaitTimer == 0
            && link->mpLinkModel != NULL && link->mpLinkFaceModel != NULL
            && link->mpLinkModel->getModelData() == mpLinkModel->getModelData()) {
            link->changeModelDataDirect(0);
        }
    }

    // Pure visual: keep the puppet out of the attention system so the local
    // player can't lock onto / talk to it. The lock-on candidate search in
    // d_attention.cpp scans actors by their attention_info.distances[], so
    // zeroing those each frame (after whatever set them up in create())
    // makes the puppet undetectable. Physical collision is already absent —
    // executePuppet never registers the puppet's CcD colliders with the
    // collision manager.
    for (int i = 0; i < fopAc_attn_MAX_e; ++i) {
        attention_info.distances[i] = 0;
    }

    // Hiding the puppet when the peer is in a different scene is handled in
    // daAlink_c::draw() (it early-returns when isPuppet() &&
    // replication::IsPeerInDifferentScene()). The scene caches are fed every
    // tick from the PlayerPose stream (which carries stage/room), so they
    // converge almost immediately and the gate fails open until then.
    // executePuppet() keeps running regardless so the transform stays current
    // and the puppet pops back in at the right place when the players reunite.

    // Match the return convention of daAlink_Execute (non-zero = OK; 0
    // would cause the actor framework to delete us).
    return 1;
}

int daAlink_c::drawPuppet() {
    // Hide the puppet only when we *positively* know the peer is in a different
    // stage/room — not merely when co-location is unconfirmed (e.g. the peer's
    // first pose hasn't arrived yet). Otherwise a freshly spawned puppet would
    // be invisible until the scene caches converge. executePuppet() keeps
    // running every tick so the puppet pops back in the moment they reunite.
    if (dusk::net::replication::IsPeerInDifferentScene()) {
        return 1;
    }

    // Set up the actor's tev struct from the environment light the same way
    // daAlink_c::draw() does for the human form, then draw the four pieces
    // executePuppet() keeps posed: body, hand, hat, face. Everything else the
    // real draw() touches (sword/shield/held-item/wolf/clothes-change/particles/
    // shadow/sight) doesn't apply to a pure-visual puppet.
    g_env_light.settingTevStruct(10, &current.pos, &tevStr);
    initTevCustomColor();

    modelDraw(mpLinkModel, 0);
    if (mpLinkHandModel != NULL) {
        modelDraw(mpLinkHandModel, 0);
    }
    if (mpLinkHatModel != NULL) {
        modelDraw(mpLinkHatModel, 0);
    }
    if (mpLinkFaceModel != NULL) {
        modelDraw(mpLinkFaceModel, 0);
    }

    return 1;
}

#endif  // TARGET_PC && DUSK_ENABLE_MULTIPLAYER
