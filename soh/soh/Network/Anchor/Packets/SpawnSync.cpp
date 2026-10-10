#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/OTRGlobals.h"

#include <string>
#include <vector>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Encount1/z_en_encount1.h"
#include "src/overlays/actors/ovl_En_Niw/z_en_niw.h"
#include "src/overlays/actors/ovl_En_Attack_Niw/z_en_attack_niw.h"
extern PlayState* gPlayState;
void func_809B59B0(EnAttackNiw* this_, PlayState* play);
}

std::string AnchorEnemySync_GetKey(Actor* actor);
bool AnchorEnemySync_IsHandled(Actor* actor);
bool AnchorEnemySync_IsKeyDefeated(const std::string& key);

/**
 * Spawn Sync (enemies that appear out of nowhere)
 *
 * Most enemies are placed in the level data, so every player's game already has the same ones and enemy sync can match
 * them up. Some are made on the spot instead, at random spots around the player:
 *   - Stalchildren (Hyrule Field at night), Wolfos, Leevers and Tektites, made by invisible enemy spawners
 *   - The swarm of angry cuccos that attacks after you hit a cucco too many times
 * Each game made its own, so everyone fought different ones.
 *
 * Now, whenever one of these appears in your game, everyone else in the scene gets a copy of it at the same spot. The
 * copy has the same identity (scene + id + params + spawn position) as yours, so all the rest of enemy sync works on it
 * like any other enemy: it moves with whoever is closest, hits are replayed, and it dies for everyone.
 *
 * Each game's spawners keep working around their own player, so Stalchildren still pop up around each of you, and
 * everyone sees all of them. Outside Hyrule Field (fixed spawners like the Wolfos in the Sacred Forest Meadow) a copy
 * counts against your own spawner too, so a group arriving together doesn't get double the enemies.
 *
 * When one goes away without being defeated (a Stalchild burrowing at sunrise, a cucco flying off), it goes away for
 * everyone. Players entering the scene later are sent copies of the ones still around.
 *
 * Copies of angry cuccos chase the player who made them angry, in everyone's game.
 *
 * ENEMY_SPAWNED   - Broadcast to the room when one appears in your game (or sent directly to someone who just arrived)
 * ENEMY_DESPAWNED - Broadcast to the room when one goes away without being defeated
 */

namespace {

struct SpawnSyncData {
    bool shared = false;       // One of the spawned-on-the-spot actors that everyone gets a copy of
    bool isCopy = false;       // Our copy of someone else's (they made it)
    bool announced = false;    // We've told everyone about our own
    bool despawnSent = false;  // We've already told everyone it went away
    uint32_t ownerClientId = 0;
    std::string spawnerKey;    // The spawner or cucco it came from
    Vec3f spawnPos = { 0.0f, 0.0f, 0.0f };
    Vec3s spawnRot = { 0, 0, 0 };
    s16 params = 0;
};
static ObjectExtension::Register<SpawnSyncData> SpawnSyncDataRegister;

static Actor* sUpdatingSpawner = nullptr; // The spawner or cucco running its update right now
static bool sSpawningCopy = false;        // We're making a copy right now (don't announce it)
static uint32_t sCopyOwner = 0;
static uint32_t sCopyOwnerForSetup = 0; // Owner of the copy just made, for setting it up after spawning
static bool sApplyingDespawn = false;     // We're removing one because someone else's went away

bool IsSpawnerActor(Actor* actor) {
    return actor->id == ACTOR_EN_ENCOUNT1 || actor->id == ACTOR_EN_NIW;
}

// The kinds of actor each spawner makes that get shared
bool IsSharedSpawnType(s16 actorId, s16 spawnerId) {
    if (spawnerId == ACTOR_EN_ENCOUNT1) {
        return actorId == ACTOR_EN_SKB || actorId == ACTOR_EN_WF || actorId == ACTOR_EN_REEBA ||
               actorId == ACTOR_EN_TITE;
    }
    if (spawnerId == ACTOR_EN_NIW) {
        return actorId == ACTOR_EN_ATTACK_NIW;
    }
    return false;
}

bool ShouldSyncSpawns() {
    return Anchor::Instance != nullptr && Anchor::Instance->roomState.syncEnemies && Anchor::Instance->IsSaveLoaded();
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

// Any loaded, live actor with this key (spawned actors can be enemies or, for angry cuccos, other categories)
Actor* FindActorByKey(const std::string& key, s16 actorId) {
    for (s32 category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[category].head; actor != NULL; actor = actor->next) {
            if (actor->update == NULL || actor->id != actorId) {
                continue;
            }
            if (AnchorEnemySync_GetKey(actor) == key) {
                return actor;
            }
        }
    }
    return nullptr;
}

// Being removed because its room is unloading (moving to another room), not because it left
bool IsRoomUnload(Actor* actor) {
    return actor->room >= 0 && actor->room != gPlayState->roomCtx.curRoom.num &&
           actor->room != gPlayState->roomCtx.prevRoom.num;
}

nlohmann::json DescribeSpawn(Actor* actor, SpawnSyncData* data) {
    nlohmann::json spawn;
    spawn["key"] = AnchorEnemySync_GetKey(actor);
    spawn["id"] = actor->id;
    spawn["params"] = data->params;
    spawn["pos"] = { data->spawnPos.x, data->spawnPos.y, data->spawnPos.z };
    spawn["rot"] = { data->spawnRot.x, data->spawnRot.y, data->spawnRot.z };
    spawn["spawnerKey"] = data->spawnerKey;
    spawn["owner"] = data->ownerClientId;
    return spawn;
}

// Make our copy of another player's spawned actor
void SpawnCopy(const nlohmann::json& spawn, uint32_t fallbackOwner) {
    std::string key = spawn.value("key", std::string());
    s16 actorId = spawn.value("id", (s16)-1);
    std::vector<f32> pos = spawn.value("pos", std::vector<f32>{});
    std::vector<s16> rot = spawn.value("rot", std::vector<s16>{});
    if (key.empty() || actorId < 0 || pos.size() != 3 || rot.size() != 3) {
        return;
    }

    // Already have it (or it's already dead here)
    if (AnchorEnemySync_IsKeyDefeated(key) || FindActorByKey(key, actorId) != nullptr) {
        return;
    }

    // Find our copy of the spawner or cucco it came from
    std::string spawnerKey = spawn.value("spawnerKey", std::string());
    Actor* spawner = nullptr;
    if (!spawnerKey.empty()) {
        s16 spawnerId = actorId == ACTOR_EN_ATTACK_NIW ? ACTOR_EN_NIW : ACTOR_EN_ENCOUNT1;
        spawner = FindActorByKey(spawnerKey, spawnerId);
    }

    // Angry cuccos look up their cucco every frame, so they can't exist without one
    if (actorId == ACTOR_EN_ATTACK_NIW && spawner == nullptr) {
        return;
    }

    // Hyrule Field's spawner makes enemies around each player wherever they are, so copies don't count against ours.
    // Everywhere else the spawner is a fixed spot, and a copy should fill one of its slots.
    bool countAgainstSpawner =
        spawner != nullptr && (actorId == ACTOR_EN_ATTACK_NIW || gPlayState->sceneNum != SCENE_HYRULE_FIELD);

    sSpawningCopy = true;
    sCopyOwner = spawn.value("owner", fallbackOwner);
    sCopyOwnerForSetup = sCopyOwner;
    s16 params = spawn.value("params", (s16)0);
    Actor* copy;
    if (countAgainstSpawner) {
        copy = Actor_SpawnAsChild(&gPlayState->actorCtx, spawner, gPlayState, actorId, pos[0], pos[1], pos[2], rot[0],
                                  rot[1], rot[2], params);
    } else {
        copy = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId, pos[0], pos[1], pos[2], rot[0], rot[1], rot[2],
                           params);
    }
    sSpawningCopy = false;

    if (copy == nullptr) {
        return;
    }

    // Angry cuccos normally start by diving in at the camera of the player they're after. In our game that would be
    // our camera, so our copies skip that and go straight to chasing (the same state they switch to after the dive)
    if (actorId == ACTOR_EN_ATTACK_NIW && copy->init == NULL) {
        EnAttackNiw* cucco = (EnAttackNiw*)copy;
        Player* owner = GetDummyInScene(sCopyOwnerForSetup);
        cucco->unk_2D4 = owner != nullptr ? Actor_WorldYawTowardActor(copy, &owner->actor) : copy->world.rot.y;
        cucco->unk_2D0 = copy->world.rot.x - 3000.0f;
        cucco->unk_2DC = 0.0f;
        cucco->unk_284 = 0.0f;
        cucco->unk_27C = 0.0f;
        cucco->unk_254 = cucco->unk_256 = cucco->unk_258 = cucco->unk_25A = 0;
        cucco->unk_25C = 0x64;
        copy->gravity = -0.2f;
        cucco->unk_2E0 = 5.0f;
        cucco->unk_288 = 0.0f;
        cucco->actionFunc = func_809B59B0;
    }

    if (!countAgainstSpawner) {
        return;
    }

    // It now fills one of the spawner's slots, just like one it made itself (it gives the slot back when it goes)
    if (actorId == ACTOR_EN_ATTACK_NIW) {
        ((EnNiw*)spawner)->unk_296++;
    } else {
        EnEncount1* encount = (EnEncount1*)spawner;
        encount->curNumSpawn++;
        encount->totalNumSpawn++;
    }
}

} // namespace

// Called from enemy sync when someone arrives in our scene: send them copies of what we've made that's still around
void Anchor::SendPacket_SpawnStateTo(uint32_t clientId) {
    if (!ShouldSyncSpawns()) {
        return;
    }

    nlohmann::json spawns = nlohmann::json::array();
    for (s32 category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[category].head; actor != NULL; actor = actor->next) {
            if (actor->update == NULL) {
                continue;
            }
            SpawnSyncData* data = ObjectExtension::GetInstance().Get<SpawnSyncData>(actor);
            if (data == nullptr || !data->shared || data->isCopy || !data->announced ||
                AnchorEnemySync_IsHandled(actor)) {
                continue;
            }
            spawns.push_back(DescribeSpawn(actor, data));
        }
    }

    if (spawns.empty()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_SPAWNED;
    payload["targetClientId"] = clientId;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["spawns"] = spawns;
    SendJsonToRemote(payload);
}

void Anchor::RegisterSpawnSyncHooks() {
    // Note which spawner (or cucco) is running right now, so we know where new actors come from
    COND_HOOK(OnActorUpdateBegin, isConnected, [&](void* refActor, void** playerOverride) {
        Actor* actor = (Actor*)refActor;

        if (IsSpawnerActor(actor)) {
            sUpdatingSpawner = actor;
            return;
        }

        // Our copies of someone's angry cuccos go after them, not us
        if (actor->id == ACTOR_EN_ATTACK_NIW) {
            SpawnSyncData* data = ObjectExtension::GetInstance().Get<SpawnSyncData>(actor);
            if (data != nullptr && data->isCopy) {
                Player* owner = GetDummyInScene(data->ownerClientId);
                if (owner != nullptr) {
                    // The game worked out the distance and direction to us before this update; aim them at the owner
                    // instead, or they'd fly at us while "the player" is their character
                    Actor* target = &owner->actor;
                    actor->xzDistToPlayer = Actor_WorldDistXZToActor(actor, target);
                    actor->yDistToPlayer = Actor_HeightDiff(actor, target);
                    actor->xyzDistToPlayerSq = SQ(actor->xzDistToPlayer) + SQ(actor->yDistToPlayer);
                    actor->yawTowardsPlayer = Actor_WorldYawTowardActor(actor, target);
                    *playerOverride = target;
                }
            }
        }
    });

    COND_HOOK(OnActorUpdateEnd, isConnected, [&](void* refActor) {
        if (refActor == sUpdatingSpawner) {
            sUpdatingSpawner = nullptr;
        }
    });

    // Angry cuccos fly off once they're out of view of the player they're after. Our copies are after someone else, so
    // our view doesn't matter: they stay until the real ones leave in that player's game.
    COND_VB_SHOULD(VB_ATTACK_CUCCO_LEAVE_OFFSCREEN, isConnected, {
        Actor* cucco = va_arg(args, Actor*);
        SpawnSyncData* data = ObjectExtension::GetInstance().Get<SpawnSyncData>(cucco);
        if (data != nullptr && data->isCopy && GetDummyInScene(data->ownerClientId) != nullptr) {
            *should = false;
        }
    });

    // Tag new spawned-on-the-spot actors before they initialize (they're still exactly where they were made)
    COND_HOOK(ShouldActorInit, isConnected, [&](void* refActor, bool* should) {
        Actor* actor = (Actor*)refActor;
        if (gPlayState == nullptr) {
            return;
        }

        bool fromSpawner = sUpdatingSpawner != nullptr && IsSharedSpawnType(actor->id, sUpdatingSpawner->id);
        if (!sSpawningCopy && !fromSpawner) {
            return;
        }

        SpawnSyncData data;
        data.shared = true;
        data.isCopy = sSpawningCopy;
        data.ownerClientId = sSpawningCopy ? sCopyOwner : ownClientId;
        data.spawnPos = actor->home.pos;
        data.spawnRot = actor->home.rot;
        data.params = actor->params;
        if (actor->parent != nullptr) {
            data.spawnerKey = AnchorEnemySync_GetKey(actor->parent);
        } else if (fromSpawner) {
            data.spawnerKey = AnchorEnemySync_GetKey(sUpdatingSpawner);
        }
        ObjectExtension::GetInstance().Set<SpawnSyncData>(actor, std::move(data));
    });

    // After its first update, tell everyone about one we made (by then we know it really came into being)
    COND_HOOK(OnActorUpdate, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        SpawnSyncData* data = ObjectExtension::GetInstance().Get<SpawnSyncData>(actor);
        if (data == nullptr || !data->shared || data->isCopy || data->announced) {
            return;
        }

        data->announced = true;
        if (!ShouldSyncSpawns()) {
            return;
        }

        // Spawned by Actor_SpawnAsChild, the parent is set after init: pick up the spawner's key now if we missed it
        if (data->spawnerKey.empty() && actor->parent != nullptr) {
            data->spawnerKey = AnchorEnemySync_GetKey(actor->parent);
        }

        nlohmann::json payload;
        payload["type"] = ENEMY_SPAWNED;
        payload["sceneNum"] = gPlayState->sceneNum;
        payload["spawns"] = nlohmann::json::array({ DescribeSpawn(actor, data) });
        SendJsonToRemote(payload);
    });

    // One went away without being defeated (burrowed, flew off, wandered too far): it goes for everyone
    COND_HOOK(OnActorKill, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        SpawnSyncData* data = ObjectExtension::GetInstance().Get<SpawnSyncData>(actor);
        if (data == nullptr || !data->shared || data->despawnSent || sApplyingDespawn || gPlayState == nullptr) {
            return;
        }

        data->despawnSent = true;
        // Only the game that made it decides when it's gone. Our copies can leave on their own here (wandering out of
        // our view, too far from us...) while they're still very much around in their real game.
        if (!ShouldSyncSpawns() || AnchorEnemySync_IsHandled(actor) || IsRoomUnload(actor) || data->isCopy ||
            !data->announced) {
            return;
        }

        nlohmann::json payload;
        payload["type"] = ENEMY_DESPAWNED;
        payload["sceneNum"] = gPlayState->sceneNum;
        payload["key"] = AnchorEnemySync_GetKey(actor);
        payload["id"] = actor->id;
        SendJsonToRemote(payload);
    });
}

void Anchor::HandlePacket_EnemySpawned(nlohmann::json payload) {
    if (!ShouldSyncSpawns() || payload.value("sceneNum", (s16)-1) != gPlayState->sceneNum ||
        !payload.contains("spawns")) {
        return;
    }

    uint32_t sender = payload.value("clientId", (uint32_t)0);
    for (auto& spawn : payload["spawns"]) {
        SpawnCopy(spawn, sender);
    }
}

void Anchor::HandlePacket_EnemyDespawned(nlohmann::json payload) {
    if (!ShouldSyncSpawns() || payload.value("sceneNum", (s16)-1) != gPlayState->sceneNum) {
        return;
    }

    Actor* actor = FindActorByKey(payload.value("key", std::string()), payload.value("id", (s16)-1));
    if (actor == nullptr || AnchorEnemySync_IsHandled(actor)) {
        return;
    }

    SpawnSyncData* data = ObjectExtension::GetInstance().Get<SpawnSyncData>(actor);
    if (data == nullptr || !data->shared) {
        return;
    }

    // Don't pull one out from under us while it's grabbing us
    Player* player = GET_PLAYER(gPlayState);
    if (player->actor.parent == actor || actor->child == &player->actor) {
        return;
    }

    sApplyingDespawn = true;
    data->despawnSent = true;
    Actor_Kill(actor);
    sApplyingDespawn = false;
}
