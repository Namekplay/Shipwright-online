#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "src/overlays/effects/ovl_Effect_Ss_HitMark/z_eff_ss_hitmark.h"
extern PlayState* gPlayState;
}

/**
 * HIT_EFFECT
 *
 * The sparks, hit marks and clink sounds your attacks make when they hit something: your sword hitting a rock, a
 * wall, a metal or wooden object, a slingshot seed bouncing off something hard, and so on. These only happen in the
 * game of the player who attacked, so on its own another player swinging at a rock looks like nothing happened.
 * Each one is sent to the other players in the same scene, who spawn the same spark and play the same sound there.
 *
 * Hits on enemies are not sent here: those are replayed as real hits on the other players' copy of the enemy (see
 * EnemySync), which already makes the enemy's own hit effects.
 */

namespace {

// Our own character, or something our character fired or threw (slingshot seeds, arrows, boomerang, hookshot)
bool IsOwnedByLocalPlayer(Actor* actor) {
    if (actor == nullptr || gPlayState == nullptr) {
        return false;
    }

    Actor* self = &GET_PLAYER(gPlayState)->actor;
    return actor == self || actor->parent == self;
}

} // namespace

void Anchor::RegisterHitEffectHooks() {
    COND_HOOK(OnHitEffect, isConnected,
              [&](void* refAttacker, void* refVictim, int16_t hitmark, uint8_t sparks, uint16_t sfxId, float x,
                  float y, float z) {
                  if (isProcessingIncomingPacket || !IsSaveLoaded()) {
                      return;
                  }

                  Actor* attacker = (Actor*)refAttacker;
                  Actor* victim = (Actor*)refVictim;
                  if (!IsOwnedByLocalPlayer(attacker) || AnchorEnemySync_WillReplayHit(victim)) {
                      return;
                  }

                  SendPacket_HitEffect(hitmark, sparks, sfxId, x, y, z);
              });
}

void Anchor::SendPacket_HitEffect(s16 hitmark, u8 sparks, u16 sfxId, f32 x, f32 y, f32 z) {
    if (!IsSaveLoaded() || (hitmark < 0 && !sparks && sfxId == 0)) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = HIT_EFFECT;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["hitmark"] = hitmark;
    payload["sparks"] = sparks;
    payload["sfxId"] = sfxId;
    payload["pos"] = { x, y, z };
    payload["quiet"] = true;

    // Only players standing in this scene can see it
    for (auto& [clientId, client] : clients) {
        if (client.sceneNum == gPlayState->sceneNum && client.online && client.isSaveLoaded && !client.self) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_HitEffect(nlohmann::json payload) {
    if (!IsSaveLoaded() || payload.value("sceneNum", (s16)-1) != gPlayState->sceneNum) {
        return;
    }

    auto posList = payload.value("pos", std::vector<f32>{});
    if (posList.size() != 3) {
        return;
    }

    Vec3f pos = { posList[0], posList[1], posList[2] };
    s16 hitmark = payload.value("hitmark", (s16)-1);
    u16 sfxId = payload.value("sfxId", (u16)0);

    if (hitmark >= EFFECT_HITMARK_WHITE && hitmark <= EFFECT_HITMARK_METAL) {
        EffectSsHitMark_SpawnFixedScale(gPlayState, hitmark, &pos);
    }

    if (payload.value("sparks", (u8)0)) {
        CollisionCheck_SpawnShieldParticles(gPlayState, &pos);
    }

    if (sfxId != 0) {
        SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &pos, 20, sfxId);
    }
}
