#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/OTRGlobals.h"

#include <cmath>
#include <string>
#include <vector>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Niw/z_en_niw.h"
#include "src/overlays/actors/ovl_En_Go/z_en_go.h"
#include "src/overlays/actors/ovl_En_Go2/z_en_go2.h"
extern PlayState* gPlayState;
}

std::string AnchorEnemySync_GetKey(Actor* actor);

/**
 * NPC Sync (cuccos and rolling Gorons)
 *
 * These aren't enemies, but they run around on their own, so every player saw them in different places.
 *
 * Movement: cuccos and rolling Gorons use the same movement sync as enemies - the player standing closest to one runs
 * it and sends where it is; everyone else's copy follows. Rolling Gorons also send which point of their route they're
 * heading to, so they take the same path, and they're only lined up while they're rolling in both games (stopping one
 * to talk to it or get its reward still happens in your own game).
 *
 * Cucco hits: hitting a cucco makes it squawk, drop feathers and flee in everyone's game. The hit only counts toward
 * making it angry in the game of the player who hit it, so only that player sets off the attack (and the camera shot
 * of the angry cucco). The attacking swarm itself is copied to everyone by Spawn Sync, and goes after that player.
 *
 * Carrying: picking up a cucco hands it to your character in everyone's game, so they see you carry it (and glide with
 * it). Letting go sends where and how fast, so it flutters off the same way.
 *
 * NPC_HIT      - Broadcast to the room when you hit a cucco
 * NPC_HELD     - Broadcast to the room when you pick up a cucco
 * NPC_RELEASED - Broadcast to the room when you throw or put down a cucco you were carrying
 */

namespace {

struct NpcSyncData {
    // Cucco hits
    bool hadHitBeforeUpdate = false; // The cucco had a hit waiting when its update started
    bool injectedHit = false;        // That hit is another player's, replayed by us
    s16 savedAngerCount = 0;         // Hits left before it gets angry, before a replayed hit (put back after)

    // Carrying
    bool heldByMe = false;
    uint32_t heldByClient = 0;

    // Rolling Gorons: whether the player running it says it's rolling
    bool remoteRolling = false;
};
static ObjectExtension::Register<NpcSyncData> NpcSyncDataRegister;

NpcSyncData* GetNpcData(Actor* actor) {
    NpcSyncData* data = ObjectExtension::GetInstance().Get<NpcSyncData>(actor);
    if (data == nullptr) {
        ObjectExtension::GetInstance().Set<NpcSyncData>(actor, NpcSyncData{});
        data = ObjectExtension::GetInstance().Get<NpcSyncData>(actor);
    }
    return data;
}

bool IsCucco(Actor* actor) {
    return actor->id == ACTOR_EN_NIW;
}

// Gorons that roll along a route: the big one in Goron City (child), Link the Goron (adult), and the small ones on
// Death Mountain Trail that blow up at the end of their route
bool IsRollingGoron(Actor* actor) {
    if (actor->id == ACTOR_EN_GO2) {
        switch (actor->params & 0x1F) {
            case GORON_CITY_ROLLING_BIG:
            case GORON_CITY_LINK:
            case GORON_DMT_ROLLING_SMALL:
                return true;
            default:
                return false;
        }
    }
    if (actor->id == ACTOR_EN_GO) {
        u16 type = actor->params & 0xF0;
        return type == 0x00 || type == 0x30;
    }
    return false;
}

bool IsRollingNow(Actor* actor) {
    return actor->speedXZ >= 1.0f;
}

bool ShouldSyncNpcs() {
    return Anchor::Instance != nullptr && Anchor::Instance->roomState.syncEnemies &&
           Anchor::Instance->roomState.syncEnemyMovement && Anchor::Instance->IsSaveLoaded();
}

Player* GetDummyInScene(uint32_t clientId) {
    auto it = Anchor::Instance->clients.find(clientId);
    if (it == Anchor::Instance->clients.end()) {
        return nullptr;
    }

    AnchorClient& client = it->second;
    if (client.self || !client.online || !client.isSaveLoaded || client.sceneNum != gPlayState->sceneNum ||
        client.player == nullptr || client.player->actor.update == NULL) {
        return nullptr;
    }

    return client.player;
}

Actor* FindCuccoByKey(const std::string& key) {
    for (s32 category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[category].head; actor != NULL; actor = actor->next) {
            if (actor->update != NULL && IsCucco(actor) && AnchorEnemySync_GetKey(actor) == key) {
                return actor;
            }
        }
    }
    return nullptr;
}

// Take a cucco out of another player's character's hands in our game
void ReleaseFromDummy(Actor* actor, NpcSyncData* data) {
    Player* dummy = GetDummyInScene(data->heldByClient);
    if (dummy != nullptr && actor->parent == &dummy->actor) {
        if (dummy->heldActor == actor) {
            dummy->heldActor = NULL;
        }
        if (dummy->actor.child == actor) {
            dummy->actor.child = NULL;
        }
    }

    if (data->heldByClient != 0 && actor->parent != NULL && actor->parent->category != ACTORCAT_PLAYER) {
        actor->parent = NULL;
    }
    data->heldByClient = 0;
}

} // namespace

// Movement sync hooks (called from EnemySync)

bool AnchorNpcSync_IsMovementSynced(Actor* actor) {
    return IsCucco(actor) || IsRollingGoron(actor);
}

bool AnchorNpcSync_CanApplyMovement(Actor* actor) {
    if (IsRollingGoron(actor)) {
        // Only line it up while it's rolling here and there; stopped, it's busy with that player
        NpcSyncData* data = ObjectExtension::GetInstance().Get<NpcSyncData>(actor);
        return data != nullptr && data->remoteRolling && IsRollingNow(actor);
    }
    return true;
}

void AnchorNpcSync_WriteMovementExtra(Actor* actor, nlohmann::json& entry) {
    if (actor->id == ACTOR_EN_GO2 && IsRollingGoron(actor)) {
        EnGo2* goron = (EnGo2*)actor;
        entry.push_back(IsRollingNow(actor) ? 1 : 0);
        entry.push_back(goron->waypoint);
        entry.push_back(goron->reverse);
        entry.push_back(actor->speedXZ);
    } else if (actor->id == ACTOR_EN_GO && IsRollingGoron(actor)) {
        EnGo* goron = (EnGo*)actor;
        entry.push_back(IsRollingNow(actor) ? 1 : 0);
        entry.push_back(goron->unk_218);
        entry.push_back(0);
        entry.push_back(actor->speedXZ);
    }
}

void AnchorNpcSync_ReadMovementExtra(Actor* actor, const nlohmann::json& entry) {
    if (!IsRollingGoron(actor)) {
        return;
    }

    NpcSyncData* data = GetNpcData(actor);
    data->remoteRolling = entry.size() >= 10 && entry[6].get<s32>() != 0;
    if (!data->remoteRolling || !IsRollingNow(actor)) {
        return;
    }

    // Same point of the route, same direction, same speed
    if (actor->id == ACTOR_EN_GO2) {
        EnGo2* goron = (EnGo2*)actor;
        goron->waypoint = entry[7].get<s8>();
        goron->reverse = entry[8].get<u8>();
    } else {
        EnGo* goron = (EnGo*)actor;
        goron->unk_218 = entry[7].get<s16>();
    }
    actor->speedXZ = entry[9].get<f32>();
}

void Anchor::RegisterNpcSyncHooks() {
    // Before a cucco updates: put another player's hit on it, and note whether it has a hit waiting
    COND_ID_HOOK(OnActorUpdateBegin, ACTOR_EN_NIW, isConnected, [&](void* refActor, void** playerOverride) {
        EnNiw* cucco = (EnNiw*)refActor;
        NpcSyncData* data = GetNpcData(&cucco->actor);
        data->hadHitBeforeUpdate = (cucco->collider.base.acFlags & AC_HIT) != 0;
        if (data->injectedHit) {
            data->savedAngerCount = cucco->unk_2A4;
        }
    });

    // After a cucco updates: if a hit was taken this update, either it was ours (tell everyone) or it was a replayed
    // one (it doesn't count toward making this cucco angry here). Also notice it being picked up or let go.
    COND_ID_HOOK(OnActorUpdateEnd, ACTOR_EN_NIW, isConnected, [&](void* refActor) {
        EnNiw* cucco = (EnNiw*)refActor;
        Actor* actor = &cucco->actor;
        NpcSyncData* data = GetNpcData(actor);
        bool hitTaken = data->hadHitBeforeUpdate && !(cucco->collider.base.acFlags & AC_HIT);

        if (data->injectedHit) {
            if (hitTaken) {
                cucco->unk_2A4 = data->savedAngerCount;
            } else {
                // It couldn't react right now (being carried, already angry...): drop the replayed hit
                cucco->collider.base.acFlags &= ~AC_HIT;
            }
            data->injectedHit = false;
        } else if (hitTaken && ShouldSyncNpcs()) {
            SendPacket_NpcHit(actor);
        }

        if (!ShouldSyncNpcs()) {
            return;
        }

        Player* self = GET_PLAYER(gPlayState);
        if (!data->heldByMe && actor->parent == &self->actor) {
            data->heldByMe = true;
            SendPacket_NpcHeld(actor);
        } else if (data->heldByMe && actor->parent != &self->actor) {
            data->heldByMe = false;
            SendPacket_NpcReleased(actor);
        }
    });

    // A cucco another player is carrying in our game: let it drop if their character left or let go
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_NIW, isConnected, [&](void* refActor, bool* should) {
        Actor* actor = (Actor*)refActor;
        NpcSyncData* data = ObjectExtension::GetInstance().Get<NpcSyncData>(actor);
        if (data == nullptr || data->heldByClient == 0) {
            return;
        }

        Player* dummy = GetDummyInScene(data->heldByClient);
        if (dummy == nullptr || actor->parent != &dummy->actor || dummy->heldActor != actor) {
            ReleaseFromDummy(actor, data);
        }
    });

    COND_ID_HOOK(OnActorDestroy, ACTOR_EN_NIW, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        NpcSyncData* data = ObjectExtension::GetInstance().Get<NpcSyncData>(actor);
        if (data != nullptr && data->heldByClient != 0) {
            ReleaseFromDummy(actor, data);
        }
    });
}

void Anchor::SendPacket_NpcHit(Actor* actor) {
    nlohmann::json payload;
    payload["type"] = NPC_HIT;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = AnchorEnemySync_GetKey(actor);
    payload["quiet"] = true;
    SendJsonToRemote(payload);
}

void Anchor::SendPacket_NpcHeld(Actor* actor) {
    nlohmann::json payload;
    payload["type"] = NPC_HELD;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = AnchorEnemySync_GetKey(actor);
    SendJsonToRemote(payload);
}

void Anchor::SendPacket_NpcReleased(Actor* actor) {
    nlohmann::json payload;
    payload["type"] = NPC_RELEASED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = AnchorEnemySync_GetKey(actor);
    payload["pos"] = { actor->world.pos.x, actor->world.pos.y, actor->world.pos.z };
    payload["rotY"] = actor->world.rot.y;
    payload["speedXZ"] = actor->speedXZ;
    payload["velocityY"] = actor->velocity.y;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_NpcHit(nlohmann::json payload) {
    if (!ShouldSyncNpcs() || payload.value("sceneNum", (s16)-1) != gPlayState->sceneNum) {
        return;
    }

    Actor* actor = FindCuccoByKey(payload.value("key", std::string()));
    if (actor == nullptr) {
        return;
    }

    // The cucco reacts to this on its next update, the same as when a sword or a rock hits it
    EnNiw* cucco = (EnNiw*)actor;
    NpcSyncData* data = GetNpcData(actor);
    cucco->collider.base.acFlags |= AC_HIT;
    data->injectedHit = true;
}

void Anchor::HandlePacket_NpcHeld(nlohmann::json payload) {
    if (!ShouldSyncNpcs() || payload.value("sceneNum", (s16)-1) != gPlayState->sceneNum ||
        !payload.contains("clientId")) {
        return;
    }

    Actor* actor = FindCuccoByKey(payload.value("key", std::string()));
    uint32_t clientId = payload["clientId"].get<uint32_t>();
    Player* dummy = GetDummyInScene(clientId);
    Player* self = GET_PLAYER(gPlayState);
    if (actor == nullptr || dummy == nullptr || dummy->heldActor != NULL || actor->parent == &self->actor) {
        return; // Can't show it in their hands (or we're holding it ourselves)
    }

    // Hand it to their character, the same way the game does when you lift something
    NpcSyncData* data = GetNpcData(actor);
    data->heldByClient = clientId;
    dummy->heldActor = actor;
    dummy->actor.child = actor;
    actor->parent = &dummy->actor;
    actor->speedXZ = 0.0f;
    actor->velocity.y = 0.0f;
}

void Anchor::HandlePacket_NpcReleased(nlohmann::json payload) {
    if (!ShouldSyncNpcs() || payload.value("sceneNum", (s16)-1) != gPlayState->sceneNum) {
        return;
    }

    Actor* actor = FindCuccoByKey(payload.value("key", std::string()));
    if (actor == nullptr) {
        return;
    }

    NpcSyncData* data = GetNpcData(actor);
    if (data->heldByClient == 0) {
        return;
    }
    ReleaseFromDummy(actor, data);

    // Let go exactly where and how they did; the cucco's own code then flutters down like normal
    auto pos = payload.value("pos", std::vector<f32>{});
    if (pos.size() == 3) {
        actor->world.pos.x = pos[0];
        actor->world.pos.y = pos[1];
        actor->world.pos.z = pos[2];
    }
    actor->world.rot.y = payload.value("rotY", actor->world.rot.y);
    actor->speedXZ = payload.value("speedXZ", 0.0f);
    actor->velocity.y = payload.value("velocityY", 0.0f);
}
