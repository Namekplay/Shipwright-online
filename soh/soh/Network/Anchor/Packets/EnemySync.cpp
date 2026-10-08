#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/OTRGlobals.h"

#include <cmath>
#include <map>
#include <string>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * Enemy Sync (shared kills)
 *
 * When any player defeats an enemy, that same enemy is removed from the game of every other player who is in the same
 * scene, and players who enter a scene later are told which enemies are already dead there.
 *
 * Enemies are identified across games by: scene + actor id + params + spawn (home) position. Enemies placed in the
 * scene/room data spawn at the exact same position for everyone, so this key matches between games. Enemies spawned
 * randomly at runtime (Hyrule Field Stalchildren, Leevers, etc.) won't match and are simply left alone.
 *
 * The list of dead enemies lasts while you stay in the scene (moving between rooms of a dungeon included) and is
 * cleared when you go to a different scene, so enemies respawn the same way they normally would.
 *
 * ENEMY_DEFEATED      - Broadcast to the room when you defeat an enemy
 * REQUEST_ENEMY_STATE - Broadcast to the room when you enter a scene, asking for that scene's dead enemies
 * ENEMY_STATE         - Reply sent directly to the player who asked
 */

namespace {

struct EnemySyncData {
    std::string key;
    bool handled = false; // Already defeated in this game (by us or by a sync), don't touch it again
};
static ObjectExtension::Register<EnemySyncData> EnemySyncDataRegister;

// key -> silent (true when learned from ENEMY_STATE on scene entry, so the enemy disappears without an effect)
static std::map<std::string, bool> sDefeatedEnemies;
static s16 sEnemySyncSceneNum = -1;
static Vec3f sZeroVec = { 0.0f, 0.0f, 0.0f };

std::string MakeEnemyKey(Actor* actor) {
    return std::to_string(gPlayState->sceneNum) + ":" + std::to_string(actor->id) + ":" +
           std::to_string((u16)actor->params) + ":" + std::to_string(lroundf(actor->home.pos.x)) + ":" +
           std::to_string(lroundf(actor->home.pos.y)) + ":" + std::to_string(lroundf(actor->home.pos.z));
}

bool KeyIsForScene(const std::string& key, s16 sceneNum) {
    std::string prefix = std::to_string(sceneNum) + ":";
    return key.rfind(prefix, 0) == 0;
}

// Enemies that are tied to cutscenes, puzzles or unique rewards. Killing these out from under another player could
// soft-lock them or take away something they need, so they are never synced.
bool IsEnemySyncExcluded(Actor* actor) {
    switch (actor->id) {
        case ACTOR_EN_IK:          // Iron Knuckle (Nabooru cutscene)
        case ACTOR_EN_TORCH2:      // Dark Link
        case ACTOR_EN_PO_SISTERS:  // Forest Temple Poe sisters
        case ACTOR_EN_PO_FIELD:    // Big Poes (bottle reward)
        case ACTOR_EN_SKJ:         // Skull Kid
        case ACTOR_EN_DNS:         // Business scrubs
        case ACTOR_EN_HINTNUTS:    // Deku Tree 2-3-1 scrub puzzle
            return true;
        case ACTOR_EN_SW:          // Gold Skulltulas (token reward). Regular Skullwalltulas are fine.
            return ((actor->params & 0xE000) >> 0xD) != 0;
        default:
            return false;
    }
}

bool ShouldSyncEnemies() {
    return Anchor::Instance != nullptr && Anchor::Instance->roomState.syncEnemies && Anchor::Instance->IsSaveLoaded();
}

} // namespace

void Anchor::RegisterEnemySyncHooks() {
    // Tag every enemy with its key before it initializes (home position is still the untouched spawn position here).
    // If someone already defeated it, don't let it spawn at all.
    COND_HOOK(ShouldActorInit, isConnected, [&](void* actorRef, bool* should) {
        Actor* actor = (Actor*)actorRef;
        if (actor->category != ACTORCAT_ENEMY || gPlayState == nullptr) {
            return;
        }

        std::string key = MakeEnemyKey(actor);
        bool alreadyDefeated = roomState.syncEnemies && !IsEnemySyncExcluded(actor) && sDefeatedEnemies.contains(key);
        ObjectExtension::GetInstance().Set<EnemySyncData>(actor, EnemySyncData{ key, alreadyDefeated });

        if (alreadyDefeated) {
            *should = false;
        }
    });

    // Runs after each batch of room actors is spawned. When we arrive in a new scene, forget the old scene's dead
    // enemies and ask anyone already here which enemies they've killed.
    COND_HOOK(OnSceneSpawnActors, isConnected, [&]() {
        if (gPlayState == nullptr || gPlayState->sceneNum == sEnemySyncSceneNum) {
            return;
        }

        sEnemySyncSceneNum = gPlayState->sceneNum;
        sDefeatedEnemies.clear();
        SendPacket_RequestEnemyState();
    });

    // We defeated an enemy: remember it and tell everyone else
    COND_HOOK(OnEnemyDefeat, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (!ShouldSyncEnemies() || isProcessingIncomingPacket || IsEnemySyncExcluded(actor)) {
            return;
        }

        EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
        if (data == nullptr || data->handled) {
            return;
        }

        data->handled = true;
        sDefeatedEnemies[data->key] = false;
        SendPacket_EnemyDefeated(data->key);
    });

    // Remove enemies that another player defeated
    COND_HOOK(ShouldActorUpdate, isConnected, [&](void* refActor, bool* should) {
        if (sDefeatedEnemies.empty() || !roomState.syncEnemies) {
            return;
        }

        Actor* actor = (Actor*)refActor;
        if (actor->category != ACTORCAT_ENEMY || actor->update == NULL) {
            return;
        }

        EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
        if (data == nullptr || data->handled || IsEnemySyncExcluded(actor)) {
            return;
        }

        auto it = sDefeatedEnemies.find(data->key);
        if (it == sDefeatedEnemies.end()) {
            return;
        }

        // Don't yank an enemy away while it's holding us (Like Like, ReDead, etc.); try again once we're free
        Player* player = GET_PLAYER(gPlayState);
        if (player != nullptr &&
            ((player->stateFlags2 & PLAYER_STATE2_GRABBED_BY_ENEMY) || player->actor.parent == actor)) {
            return;
        }

        data->handled = true;

        if (!it->second) {
            EffectSsDeadDb_Spawn(gPlayState, &actor->world.pos, &sZeroVec, &sZeroVec, 100, 0, 255, 255, 255, 255, 0, 0,
                                 255, 1, 9, true);
        }

        Actor_Kill(actor);
        *should = false;
    });
}

void Anchor::SendPacket_EnemyDefeated(const std::string& key) {
    if (!ShouldSyncEnemies()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_DEFEATED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = key;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_EnemyDefeated(nlohmann::json payload) {
    if (!ShouldSyncEnemies()) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    if (sceneNum != gPlayState->sceneNum) {
        return;
    }

    std::string key = payload["key"].get<std::string>();
    if (!KeyIsForScene(key, sceneNum)) {
        return;
    }

    sDefeatedEnemies.emplace(key, false);
}

void Anchor::SendPacket_RequestEnemyState() {
    if (!ShouldSyncEnemies()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = REQUEST_ENEMY_STATE;
    payload["sceneNum"] = gPlayState->sceneNum;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_RequestEnemyState(nlohmann::json payload) {
    if (!ShouldSyncEnemies() || !payload.contains("clientId")) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    if (sceneNum != gPlayState->sceneNum || sceneNum != sEnemySyncSceneNum) {
        return;
    }

    nlohmann::json keys = nlohmann::json::array();
    for (auto& [key, silent] : sDefeatedEnemies) {
        if (KeyIsForScene(key, sceneNum)) {
            keys.push_back(key);
        }
    }

    if (keys.empty()) {
        return;
    }

    nlohmann::json reply;
    reply["type"] = ENEMY_STATE;
    reply["targetClientId"] = payload["clientId"].get<uint32_t>();
    reply["sceneNum"] = sceneNum;
    reply["keys"] = keys;

    SendJsonToRemote(reply);
}

void Anchor::HandlePacket_EnemyState(nlohmann::json payload) {
    if (!ShouldSyncEnemies()) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    if (sceneNum != gPlayState->sceneNum) {
        return;
    }

    for (auto& key : payload["keys"]) {
        std::string keyStr = key.get<std::string>();
        if (KeyIsForScene(keyStr, sceneNum)) {
            // Silent: these died before we arrived, so just make them not be there
            sDefeatedEnemies.emplace(keyStr, true);
        }
    }
}
