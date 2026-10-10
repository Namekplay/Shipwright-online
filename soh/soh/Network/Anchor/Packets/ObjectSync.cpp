#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/OTRGlobals.h"

#include <cmath>
#include <set>
#include <vector>
#include <string>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "src/overlays/actors/ovl_Obj_Tsubo/z_obj_tsubo.h"
#include "src/overlays/actors/ovl_Obj_Kibako/z_obj_kibako.h"
#include "src/overlays/actors/ovl_Obj_Kibako2/z_obj_kibako2.h"
#include "src/overlays/actors/ovl_En_Ishi/z_en_ishi.h"
#include "src/overlays/actors/ovl_En_Kusa/z_en_kusa.h"
extern PlayState* gPlayState;

void ObjTsubo_WaitForObject(ObjTsubo* objTsubo, PlayState* play);
void ObjTsubo_Idle(ObjTsubo* objTsubo, PlayState* play);
void ObjTsubo_AirBreak(ObjTsubo* objTsubo, PlayState* play);
void ObjKibako_Idle(ObjKibako* objKibako, PlayState* play);
void ObjKibako_AirBreak(ObjKibako* objKibako, PlayState* play);
void ObjKibako2_Idle(ObjKibako2* objKibako2, PlayState* play);
void ObjKibako2_Break(ObjKibako2* objKibako2, PlayState* play);
void EnIshi_Wait(EnIshi* enIshi, PlayState* play);
void EnIshi_SpawnFragmentsSmall(EnIshi* enIshi, PlayState* play);
void EnIshi_SpawnFragmentsLarge(EnIshi* enIshi, PlayState* play);
void EnIshi_SpawnDustSmall(EnIshi* enIshi, PlayState* play);
void EnIshi_SpawnDustLarge(EnIshi* enIshi, PlayState* play);
void EnKusa_WaitObject(EnKusa* enKusa, PlayState* play);
void EnKusa_Main(EnKusa* enKusa, PlayState* play);
void EnKusa_SpawnFragments(EnKusa* enKusa, PlayState* play);
void EnKusa_SetupCut(EnKusa* enKusa);
}

/**
 * Object Sync (pots, grass, crates, rocks)
 *
 * When any player breaks or cuts a pot, crate, rock or bush, it breaks for every other player in the same scene (with
 * the normal break effect and sound). Players entering the scene later are told which ones are gone.
 *
 * When a player picks one up, it doesn't break for everyone else: it's handed to that player's character in their
 * games, so they see it lifted and carried. When it's thrown or put down, everyone else gets where it was let go and
 * how fast it was thrown, so it flies the same way and breaks where it lands in their game too.
 *
 * Objects are matched across games the same way enemies are: scene + actor id + params + spawn position.
 *
 * Grass that grows back on its own (the regrowing kind in fields) is cut for everyone at the same moment, then grows
 * back on its own timer like normal; it isn't remembered for players who arrive later. Everything else stays gone
 * until you leave the scene, same as the enemy kill sync.
 *
 * Item drops are shared separately (see DropSync): an object broken by a replayed break never drops anything, and one
 * another player threw that breaks on landing doesn't drop a second copy of their drop. Large crates with a Gold
 * Skulltula hidden inside are not synced, so nobody loses their chance at the token.
 *
 * OBJECT_BROKEN        - Broadcast to the room when an object breaks in your game
 * OBJECT_PICKED_UP     - Broadcast to the room when you pick an object up
 * OBJECT_RELEASED      - Broadcast to the room when you throw or put down an object you picked up
 * REQUEST_OBJECT_STATE - Broadcast to the room when you enter a scene, asking for that scene's broken objects
 * OBJECT_STATE         - Reply sent directly to the player who asked
 */

namespace {

struct ObjectSyncData {
    std::string key;
    bool handled = false;   // Already broken in this game (by us or by a sync); ignore further changes
    bool wasIntact = false; // Was it sitting in its normal untouched state at the end of the last update
    bool heldByMe = false;  // We picked it up and told everyone; tell them again when we let go
    uint32_t heldByClient = 0; // Another player is carrying it (their character holds it in our game)
    bool thrownByOther = false; // Another player threw or put it down; if it breaks, that's their break
};
static ObjectExtension::Register<ObjectSyncData> ObjectSyncDataRegister;

static std::set<std::string> sBrokenObjects;
static s16 sObjectSyncSceneNum = -1;

bool IsSyncedObjectType(Actor* actor) {
    switch (actor->id) {
        case ACTOR_OBJ_TSUBO:  // Pots
        case ACTOR_OBJ_KIBAKO: // Small crates
        case ACTOR_EN_ISHI:    // Rocks
        case ACTOR_EN_KUSA:    // Grass and bushes
            return true;
        case ACTOR_OBJ_KIBAKO2: // Large crates, except the ones hiding a Gold Skulltula
            return (actor->params & 0x8000) != 0;
        default:
            return false;
    }
}

// Grass that regrows by itself: cut it for everyone at once, but don't remember it
bool Regrows(Actor* actor) {
    return actor->id == ACTOR_EN_KUSA && (actor->params & 3) == ENKUSA_TYPE_1;
}

// True while the object is sitting untouched where it spawned
bool IsIntact(Actor* actor) {
    if (actor->update == NULL) {
        return false;
    }

    switch (actor->id) {
        case ACTOR_OBJ_TSUBO: {
            ObjTsubo* pot = (ObjTsubo*)actor;
            return pot->actionFunc == ObjTsubo_WaitForObject || pot->actionFunc == ObjTsubo_Idle;
        }
        case ACTOR_OBJ_KIBAKO:
            return ((ObjKibako*)actor)->actionFunc == ObjKibako_Idle;
        case ACTOR_OBJ_KIBAKO2:
            return ((ObjKibako2*)actor)->actionFunc == ObjKibako2_Idle && actor->draw != NULL;
        case ACTOR_EN_ISHI:
            return ((EnIshi*)actor)->actionFunc == EnIshi_Wait;
        case ACTOR_EN_KUSA: {
            EnKusa* grass = (EnKusa*)actor;
            return (grass->actionFunc == EnKusa_WaitObject || grass->actionFunc == EnKusa_Main) &&
                   !(actor->flags & ACTOR_FLAG_GRASS_DESTROYED);
        }
        default:
            return false;
    }
}

std::string MakeObjectKey(Actor* actor) {
    return std::to_string(gPlayState->sceneNum) + ":" + std::to_string(actor->id) + ":" +
           std::to_string((u16)actor->params) + ":" + std::to_string(lroundf(actor->home.pos.x)) + ":" +
           std::to_string(lroundf(actor->home.pos.y)) + ":" + std::to_string(lroundf(actor->home.pos.z));
}

bool KeyIsForScene(const std::string& key, s16 sceneNum) {
    std::string prefix = std::to_string(sceneNum) + ":";
    return key.rfind(prefix, 0) == 0;
}

bool ShouldSyncObjects() {
    return Anchor::Instance != nullptr && Anchor::Instance->roomState.syncObjects && Anchor::Instance->IsSaveLoaded();
}

// Another player's character in our game, if they're here with us right now
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

// Find the object another player is carrying in our game
Actor* FindRemoteHeldObjectByKey(const std::string& key) {
    static const u8 categories[] = { ACTORCAT_PROP, ACTORCAT_BG };

    for (u8 category : categories) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[category].head; actor != NULL; actor = actor->next) {
            if (actor->update == NULL || !IsSyncedObjectType(actor)) {
                continue;
            }

            ObjectSyncData* data = ObjectExtension::GetInstance().Get<ObjectSyncData>(actor);
            if (data != nullptr && data->heldByClient != 0 && data->key == key) {
                return actor;
            }
        }
    }

    return nullptr;
}

// Take an object out of another player's character's hands in our game. Only touches the character if it's still
// the one we handed the object to (it may have been respawned since).
void ReleaseFromDummy(Actor* actor, ObjectSyncData* data) {
    Player* dummy = GetDummyInScene(data->heldByClient);
    if (dummy != nullptr && actor->parent == &dummy->actor) {
        if (dummy->heldActor == actor) {
            dummy->heldActor = NULL;
        }
        if (dummy->actor.child == actor) {
            dummy->actor.child = NULL;
        }
    }

    actor->parent = NULL;
    data->heldByClient = 0;
    data->thrownByOther = true;
}

// Find a loaded, untouched object with this key that hasn't already been handled
Actor* FindIntactObjectByKey(const std::string& key) {
    static const u8 categories[] = { ACTORCAT_PROP, ACTORCAT_BG };

    for (u8 category : categories) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[category].head; actor != NULL; actor = actor->next) {
            if (!IsSyncedObjectType(actor)) {
                continue;
            }

            ObjectSyncData* data = ObjectExtension::GetInstance().Get<ObjectSyncData>(actor);
            if (data != nullptr && !data->handled && data->key == key && IsIntact(actor)) {
                return actor;
            }
        }
    }

    return nullptr;
}

// Break the object in our game the way the game itself would, minus the item drop
void BreakObject(Actor* actor, bool silent) {
    ObjectSyncData* data = ObjectExtension::GetInstance().Get<ObjectSyncData>(actor);
    if (data != nullptr) {
        data->handled = true;
        data->wasIntact = false;
    }

    if (silent) {
        Actor_Kill(actor);
        return;
    }

    PlayState* play = gPlayState;

    switch (actor->id) {
        case ACTOR_OBJ_TSUBO:
            ObjTsubo_AirBreak((ObjTsubo*)actor, play);
            // AirBreak counts it as a pot you broke; it wasn't you, so take that back
            gSaveContext.ship.stats.count[COUNT_POTS_BROKEN]--;
            SoundSource_PlaySfxAtFixedWorldPos(play, &actor->world.pos, 20, NA_SE_EV_POT_BROKEN);
            Actor_Kill(actor);
            break;
        case ACTOR_OBJ_KIBAKO:
            ObjKibako_AirBreak((ObjKibako*)actor, play);
            SoundSource_PlaySfxAtFixedWorldPos(play, &actor->world.pos, 20, NA_SE_EV_WOODBOX_BREAK);
            Actor_Kill(actor);
            break;
        case ACTOR_OBJ_KIBAKO2:
            ObjKibako2_Break((ObjKibako2*)actor, play);
            SoundSource_PlaySfxAtFixedWorldPos(play, &actor->world.pos, 20, NA_SE_EV_WOODBOX_BREAK);
            Actor_Kill(actor);
            break;
        case ACTOR_EN_ISHI:
            if ((actor->params & 1) == ROCK_SMALL) {
                EnIshi_SpawnFragmentsSmall((EnIshi*)actor, play);
                EnIshi_SpawnDustSmall((EnIshi*)actor, play);
                SoundSource_PlaySfxAtFixedWorldPos(play, &actor->world.pos, 20, NA_SE_EV_ROCK_BROKEN);
            } else {
                EnIshi_SpawnFragmentsLarge((EnIshi*)actor, play);
                EnIshi_SpawnDustLarge((EnIshi*)actor, play);
                SoundSource_PlaySfxAtFixedWorldPos(play, &actor->world.pos, 40, NA_SE_EV_WALL_BROKEN);
            }
            Actor_Kill(actor);
            break;
        case ACTOR_EN_KUSA:
            EnKusa_SpawnFragments((EnKusa*)actor, play);
            SoundSource_PlaySfxAtFixedWorldPos(play, &actor->world.pos, 20, NA_SE_EV_PLANT_BROKEN);
            if ((actor->params & 3) == ENKUSA_TYPE_0) {
                Actor_Kill(actor);
            } else {
                // Leave the cut stump; the regrowing kind grows back on its own timer
                EnKusa_SetupCut((EnKusa*)actor);
                actor->flags |= ACTOR_FLAG_GRASS_DESTROYED;
            }
            break;
        default:
            Actor_Kill(actor);
            break;
    }
}

} // namespace

void Anchor::RegisterObjectSyncHooks() {
    // Tag each object with its key before it initializes. If it's already broken, don't let it spawn.
    COND_HOOK(ShouldActorInit, isConnected, [&](void* actorRef, bool* should) {
        Actor* actor = (Actor*)actorRef;
        if (gPlayState == nullptr || !IsSyncedObjectType(actor)) {
            return;
        }

        std::string key = MakeObjectKey(actor);
        bool alreadyBroken = roomState.syncObjects && sBrokenObjects.contains(key);
        ObjectSyncData data;
        data.key = key;
        data.handled = alreadyBroken;
        ObjectExtension::GetInstance().Set<ObjectSyncData>(actor, std::move(data));

        if (alreadyBroken) {
            *should = false;
        }
    });

    // New scene: forget the old scene's broken objects and ask anyone already here
    COND_HOOK(OnSceneSpawnActors, isConnected, [&]() {
        if (gPlayState == nullptr || gPlayState->sceneNum == sObjectSyncSceneNum) {
            return;
        }

        sObjectSyncSceneNum = gPlayState->sceneNum;
        sBrokenObjects.clear();
        SendPacket_RequestObjectState();
    });

    // After each update, see if the object just stopped being untouched (broken, cut, or picked up)
    COND_HOOK(OnActorUpdate, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (!roomState.syncObjects || !IsSyncedObjectType(actor)) {
            return;
        }

        ObjectSyncData* data = ObjectExtension::GetInstance().Get<ObjectSyncData>(actor);
        if (data == nullptr) {
            return;
        }

        bool intact = IsIntact(actor);

        // Regrowing grass that has grown back can be cut (and synced) again
        if (intact && data->handled && Regrows(actor)) {
            data->handled = false;
        }

        if (data->wasIntact && !intact && !data->handled && !isProcessingIncomingPacket) {
            data->handled = true;
            if (!Regrows(actor)) {
                sBrokenObjects.insert(data->key);
            }

            Player* self = GET_PLAYER(gPlayState);
            if (self != nullptr && actor->parent == &self->actor) {
                // We picked it up: everyone else sees our character lift and carry it
                data->heldByMe = true;
                data->thrownByOther = false;
                SendPacket_ObjectPickedUp(data->key);
            } else {
                SendPacket_ObjectBroken(data->key, !Regrows(actor));
            }
        }

        // We let go of something we picked up: send where and how hard, so it flies the same way for everyone
        if (data->heldByMe && actor->parent == NULL) {
            data->heldByMe = false;
            SendPacket_ObjectReleased(data->key, actor);
        }

        data->wasIntact = intact;
    });

    // Something another player is carrying in our game: make sure their character is still here and still holding it.
    // If not (they left, or their character was respawned), let it drop.
    COND_HOOK(ShouldActorUpdate, isConnected, [&](void* refActor, bool* should) {
        Actor* actor = (Actor*)refActor;
        if (!IsSyncedObjectType(actor)) {
            return;
        }

        ObjectSyncData* data = ObjectExtension::GetInstance().Get<ObjectSyncData>(actor);
        if (data == nullptr || data->heldByClient == 0) {
            return;
        }

        Player* dummy = GetDummyInScene(data->heldByClient);
        if (dummy == nullptr || actor->parent != &dummy->actor || dummy->heldActor != actor) {
            ReleaseFromDummy(actor, data);
            actor->speedXZ = 0.0f;
            actor->velocity.y = 0.0f;
        }
    });

    // An object another player is carrying is going away in our game: make sure their character lets go of it, so it
    // isn't left holding something that no longer exists
    COND_HOOK(OnActorDestroy, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (!IsSyncedObjectType(actor)) {
            return;
        }

        ObjectSyncData* data = ObjectExtension::GetInstance().Get<ObjectSyncData>(actor);
        if (data != nullptr && data->heldByClient != 0) {
            ReleaseFromDummy(actor, data);
        }
    });
}

void Anchor::SendPacket_ObjectPickedUp(const std::string& key) {
    if (!ShouldSyncObjects()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = OBJECT_PICKED_UP;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = key;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ObjectPickedUp(nlohmann::json payload) {
    if (!ShouldSyncObjects() || !payload.contains("clientId")) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    std::string key = payload["key"].get<std::string>();
    if (sceneNum != gPlayState->sceneNum || !KeyIsForScene(key, sceneNum)) {
        return;
    }

    sBrokenObjects.insert(key);

    Actor* actor = FindIntactObjectByKey(key);
    if (actor == nullptr) {
        return;
    }

    ObjectSyncData* data = ObjectExtension::GetInstance().Get<ObjectSyncData>(actor);
    uint32_t clientId = payload["clientId"].get<uint32_t>();
    Player* dummy = GetDummyInScene(clientId);

    if (dummy == nullptr || dummy->heldActor != NULL) {
        // Can't show it in their hands (they aren't here in our game): it just quietly goes away, no shattering
        BreakObject(actor, true);
        return;
    }

    // Hand it to their character, the same way the game does when you lift something
    data->handled = true;
    data->wasIntact = false;
    data->heldByClient = clientId;
    dummy->heldActor = actor;
    dummy->actor.child = actor;
    actor->parent = &dummy->actor;
    actor->bgCheckFlags &= 0xFF00;
}

void Anchor::SendPacket_ObjectReleased(const std::string& key, Actor* actor) {
    if (!ShouldSyncObjects()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = OBJECT_RELEASED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = key;
    payload["pos"] = { actor->world.pos.x, actor->world.pos.y, actor->world.pos.z };
    payload["rotY"] = actor->world.rot.y;
    payload["speedXZ"] = actor->speedXZ;
    payload["velocityY"] = actor->velocity.y;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ObjectReleased(nlohmann::json payload) {
    if (!ShouldSyncObjects()) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    std::string key = payload["key"].get<std::string>();
    if (sceneNum != gPlayState->sceneNum) {
        return;
    }

    Actor* actor = FindRemoteHeldObjectByKey(key);
    if (actor == nullptr) {
        return;
    }

    ObjectSyncData* data = ObjectExtension::GetInstance().Get<ObjectSyncData>(actor);
    ReleaseFromDummy(actor, data);

    // Let go exactly where and how they did; the object's own code then flies, lands and breaks like normal
    auto pos = payload.value("pos", std::vector<f32>{});
    if (pos.size() == 3) {
        actor->world.pos.x = pos[0];
        actor->world.pos.y = pos[1];
        actor->world.pos.z = pos[2];
    }
    actor->world.rot.y = actor->shape.rot.y = payload.value("rotY", actor->world.rot.y);
    actor->speedXZ = payload.value("speedXZ", 0.0f);
    actor->velocity.y = payload.value("velocityY", 0.0f);
}

void Anchor::SendPacket_ObjectBroken(const std::string& key, bool remember) {
    if (!ShouldSyncObjects()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = OBJECT_BROKEN;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = key;
    payload["remember"] = remember;
    payload["quiet"] = true;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_ObjectBroken(nlohmann::json payload) {
    if (!ShouldSyncObjects()) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    std::string key = payload["key"].get<std::string>();
    if (sceneNum != gPlayState->sceneNum || !KeyIsForScene(key, sceneNum)) {
        return;
    }

    if (payload.value("remember", true)) {
        sBrokenObjects.insert(key);
    }

    // If it's loaded and untouched in our game, break it now. If we're holding it, leave it in our hands.
    Actor* actor = FindIntactObjectByKey(key);
    if (actor != nullptr) {
        BreakObject(actor, false);
    }
}

void Anchor::SendPacket_RequestObjectState() {
    if (!ShouldSyncObjects()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = REQUEST_OBJECT_STATE;
    payload["sceneNum"] = gPlayState->sceneNum;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_RequestObjectState(nlohmann::json payload) {
    if (!ShouldSyncObjects() || !payload.contains("clientId")) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    if (sceneNum != gPlayState->sceneNum || sceneNum != sObjectSyncSceneNum) {
        return;
    }

    nlohmann::json keys = nlohmann::json::array();
    for (const std::string& key : sBrokenObjects) {
        if (KeyIsForScene(key, sceneNum)) {
            keys.push_back(key);
        }
    }

    if (keys.empty()) {
        return;
    }

    nlohmann::json reply;
    reply["type"] = OBJECT_STATE;
    reply["targetClientId"] = payload["clientId"].get<uint32_t>();
    reply["sceneNum"] = sceneNum;
    reply["keys"] = keys;

    SendJsonToRemote(reply);
}

void Anchor::HandlePacket_ObjectState(nlohmann::json payload) {
    if (!ShouldSyncObjects()) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    if (sceneNum != gPlayState->sceneNum) {
        return;
    }

    for (auto& keyJson : payload["keys"]) {
        std::string key = keyJson.get<std::string>();
        if (!KeyIsForScene(key, sceneNum)) {
            continue;
        }

        sBrokenObjects.insert(key);

        // These broke before we got here, so they just aren't there: no effect or sound
        Actor* actor = FindIntactObjectByKey(key);
        if (actor != nullptr) {
            BreakObject(actor, true);
        }
    }
}

// True when this pot/crate/rock breaking in our game is really another player's doing (they threw it, or are carrying
// it). Their game makes and shares the item drop for it, so ours shouldn't make another.
bool AnchorObjectSync_IsOtherPlayersObject(Actor* object) {
    if (object == nullptr || !IsSyncedObjectType(object)) {
        return false;
    }

    ObjectSyncData* data = ObjectExtension::GetInstance().Get<ObjectSyncData>(object);
    return data != nullptr && (data->thrownByOther || data->heldByClient != 0);
}
