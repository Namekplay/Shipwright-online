#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ObjectExtension.h"

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Arrow/z_en_arrow.h"
extern PlayState* gPlayState;

void EnArrow_Fly(EnArrow* enArrow, PlayState* play);
}

/**
 * Projectile Sync (slingshot seeds, arrows, Deku Nuts)
 *
 * Seeds, arrows and nuts only exist in the game of the player who fired them, so on everyone else's screen a friend
 * shooting the slingshot looked like nothing came out. Now, when you fire one, where it starts and how fast and in
 * which direction it's going are sent to the other players in the same scene, and their game launches the same shot
 * from your character. It flies, drops, sticks into walls and makes its impact puff and sound like a real one.
 *
 * The copy can't hurt anything (its attack is switched off, and a copied Deku Nut doesn't make the stun flash): what
 * your shot actually hit is already sent separately as a hit (see EnemySync / HitEffectSync), so letting the copy
 * hit things too would count every hit twice. When your real shot ends (it hit something, or ran out of range), the
 * copy is ended at the same spot, so it doesn't fly through an enemy your shot stopped on.
 *
 * PROJECTILE_FIRED - Broadcast to the room when you fire a seed, arrow or nut
 * PROJECTILE_ENDED - Broadcast to the room when that shot is gone from your game
 */

namespace {

struct ProjectileData {
    bool mine = false;      // We fired it
    bool sent = false;      // We've told everyone it was fired
    bool mirror = false;    // It's a copy of someone else's shot
    uint32_t ownerClientId = 0;
    uint32_t id = 0;
};
static ObjectExtension::Register<ProjectileData> ProjectileDataRegister;

static uint32_t sNextProjectileId = 1;

bool ShouldSyncProjectiles() {
    return Anchor::Instance != nullptr && Anchor::Instance->IsSaveLoaded();
}

bool IsMirrorProjectile(Actor* actor) {
    if (actor == nullptr || actor->id != ACTOR_EN_ARROW) {
        return false;
    }
    ProjectileData* data = ObjectExtension::GetInstance().Get<ProjectileData>(actor);
    return data != nullptr && data->mirror;
}

Actor* FindMirrorProjectile(uint32_t ownerClientId, uint32_t id) {
    for (s32 category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[category].head; actor != NULL; actor = actor->next) {
            if (actor->id != ACTOR_EN_ARROW || actor->update == NULL) {
                continue;
            }
            ProjectileData* data = ObjectExtension::GetInstance().Get<ProjectileData>(actor);
            if (data != nullptr && data->mirror && data->ownerClientId == ownerClientId && data->id == id) {
                return actor;
            }
        }
    }
    return nullptr;
}

} // namespace

void Anchor::RegisterProjectileSyncHooks() {
    // A seed/arrow/nut created while our own character is updating is one we're firing
    COND_ID_HOOK(OnActorSpawn, ACTOR_EN_ARROW, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (gPlayState == nullptr || isProcessingIncomingPacket) {
            return;
        }

        Player* self = GET_PLAYER(gPlayState);
        if (self != nullptr && updatingActor == &self->actor) {
            ProjectileData data;
            data.mine = true;
            ObjectExtension::GetInstance().Set<ProjectileData>(actor, std::move(data));
        }
    });

    // The moment our shot leaves the bow/slingshot (or hand, for nuts), send it
    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_ARROW, isConnected, [&](void* refActor) {
        EnArrow* arrow = (EnArrow*)refActor;
        ProjectileData* data = ObjectExtension::GetInstance().Get<ProjectileData>(&arrow->actor);
        if (data == nullptr || !data->mine || data->sent || arrow->actionFunc != EnArrow_Fly ||
            !ShouldSyncProjectiles()) {
            return;
        }

        data->sent = true;
        data->id = sNextProjectileId++;
        SendPacket_ProjectileFired(&arrow->actor, data->id);
    });

    // Our shot is gone (hit something, stuck in a wall and faded, or flew out of range)
    COND_ID_HOOK(OnActorKill, ACTOR_EN_ARROW, isConnected, [&](void* refActor) {
        EnArrow* arrow = (EnArrow*)refActor;
        ProjectileData* data = ObjectExtension::GetInstance().Get<ProjectileData>(&arrow->actor);
        if (data == nullptr || !data->mine || !data->sent || !ShouldSyncProjectiles()) {
            return;
        }

        data->mine = false; // Only once
        bool hit = arrow->hitFlags != 0 || arrow->touchedPoly || (arrow->collider.base.atFlags & AT_HIT);
        SendPacket_ProjectileEnded(data->id, &arrow->actor.world.pos, hit);
    });

    // A copied Deku Nut hitting something must not make its own stun flash (the real nut's stun is sent as a hit)
    COND_ID_HOOK(ShouldActorInit, ACTOR_EN_M_FIRE1, isConnected, [&](void* refActor, bool* should) {
        if (IsMirrorProjectile(updatingActor)) {
            *should = false;
        }
    });
}

void Anchor::SendPacket_ProjectileFired(Actor* actor, uint32_t id) {
    nlohmann::json payload;
    payload["type"] = PROJECTILE_FIRED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["id"] = id;
    payload["params"] = actor->params;
    payload["pos"] = { actor->world.pos.x, actor->world.pos.y, actor->world.pos.z };
    payload["worldRot"] = { actor->world.rot.x, actor->world.rot.y, actor->world.rot.z };
    payload["shapeRot"] = { actor->shape.rot.x, actor->shape.rot.y, actor->shape.rot.z };
    payload["speedXZ"] = actor->speedXZ;
    payload["velocityY"] = actor->velocity.y;
    payload["timer"] = ((EnArrow*)actor)->timer;
    payload["quiet"] = true;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ProjectileFired(nlohmann::json payload) {
    if (!ShouldSyncProjectiles() || !payload.contains("clientId")) {
        return;
    }

    if (payload["sceneNum"].get<s16>() != gPlayState->sceneNum) {
        return;
    }

    uint32_t clientId = payload["clientId"].get<uint32_t>();
    auto it = clients.find(clientId);
    if (it == clients.end() || it->second.player == nullptr || it->second.player->actor.update == NULL) {
        return; // Their character isn't here with us
    }

    s16 params = payload["params"].get<s16>();
    if (params < ARROW_NORMAL_LIT || params > ARROW_NUT) {
        return;
    }

    auto pos = payload["pos"];
    auto worldRot = payload["worldRot"];
    auto shapeRot = payload["shapeRot"];

    EnArrow* arrow = (EnArrow*)Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_ARROW, pos[0].get<f32>(),
                                           pos[1].get<f32>(), pos[2].get<f32>(), worldRot[0].get<s16>(),
                                           worldRot[1].get<s16>(), worldRot[2].get<s16>(), params);
    if (arrow == nullptr) {
        return;
    }

    ProjectileData data;
    data.mirror = true;
    data.ownerClientId = clientId;
    data.id = payload["id"].get<uint32_t>();
    ObjectExtension::GetInstance().Set<ProjectileData>(&arrow->actor, std::move(data));

    // Harmless copy: it can fly, bounce and stick, but never hits anything
    arrow->collider.base.atFlags &= ~(AT_ON | AT_HIT);

    // Launch it straight away, the same way the real one was launched
    arrow->actor.shape.rot.x = shapeRot[0].get<s16>();
    arrow->actor.shape.rot.y = shapeRot[1].get<s16>();
    arrow->actor.shape.rot.z = shapeRot[2].get<s16>();
    arrow->actor.speedXZ = payload["speedXZ"].get<f32>();
    arrow->actor.velocity.y = payload["velocityY"].get<f32>();
    arrow->timer = payload.value("timer", (u8)(params >= ARROW_SEED ? 15 : 12));
    Math_Vec3f_Copy(&arrow->unk_210, &arrow->actor.world.pos);
    arrow->actionFunc = EnArrow_Fly;
}

void Anchor::SendPacket_ProjectileEnded(uint32_t id, Vec3f* pos, bool hit) {
    nlohmann::json payload;
    payload["type"] = PROJECTILE_ENDED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["id"] = id;
    payload["pos"] = { pos->x, pos->y, pos->z };
    payload["hit"] = hit;
    payload["quiet"] = true;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ProjectileEnded(nlohmann::json payload) {
    if (!ShouldSyncProjectiles() || !payload.contains("clientId")) {
        return;
    }

    if (payload["sceneNum"].get<s16>() != gPlayState->sceneNum) {
        return;
    }

    Actor* actor = FindMirrorProjectile(payload["clientId"].get<uint32_t>(), payload["id"].get<uint32_t>());
    if (actor == nullptr) {
        return; // Our copy already ended on its own
    }

    EnArrow* arrow = (EnArrow*)actor;
    bool stillFlying = arrow->actionFunc == EnArrow_Fly && arrow->hitFlags == 0 && !arrow->touchedPoly;

    if (stillFlying && payload.value("hit", false)) {
        // The real shot stopped on something (usually an enemy) that our copy would have flown straight through:
        // end it right there, with the impact a seed/nut makes
        auto pos = payload["pos"];
        arrow->actor.world.pos.x = pos[0].get<f32>();
        arrow->actor.world.pos.y = pos[1].get<f32>();
        arrow->actor.world.pos.z = pos[2].get<f32>();

        if (arrow->actor.params >= ARROW_SEED) {
            EffectSsStone1_Spawn(gPlayState, &arrow->actor.world.pos, 0);
            SoundSource_PlaySfxAtFixedWorldPos(gPlayState, &arrow->actor.world.pos, 20,
                                               arrow->actor.params == ARROW_NUT ? NA_SE_IT_DEKU
                                                                                : NA_SE_IT_SLING_REFLECT);
        } else {
            EffectSsHitMark_SpawnCustomScale(gPlayState, 0, 150, &arrow->actor.world.pos);
        }
    }

    Actor_Kill(actor);
}
