#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/OTRGlobals.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>
#include <map>
#include <string>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * Enemy Sync (shared damage and kills)
 *
 * When any player defeats an enemy, that same enemy is removed from the game of every other player who is in the same
 * scene, and players who enter a scene later are told which enemies are already dead there.
 *
 * When any player damages an enemy, the same enemy loses that much health in everyone else's game too, so players
 * fighting together wear it down together. A synced hit never takes an enemy below 1 health: the final blow always
 * happens in someone's own game, which then removes it for everyone through ENEMY_DEFEATED. That way every enemy dies
 * through its normal death code at least once, and nobody is left with a zero-health enemy that never dies.
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
 * ENEMY_DAMAGE        - Broadcast to the room when an enemy loses health in your game
 * ENEMY_MOVEMENT      - Sent ~10 times a second to players in the same scene with the positions of the enemies you
 *                       are running (see below)
 *
 * Enemy movement: every game still runs its own enemy AI, but for each enemy the player standing closest to it is in
 * charge of it. That player's game runs the enemy normally and sends out where it is and which way it faces. Everyone
 * else's copy of that enemy is pulled toward that position each frame (or snapped there if it's far off). The enemy
 * then fights whoever is closest exactly like normal, and everyone else sees it in the same place. When a different
 * player becomes the closest, control passes to them automatically.
 */

namespace {

struct EnemySyncData {
    std::string key;
    bool handled = false; // Already defeated in this game (by us or by a sync), don't touch it again
    s16 lastHealth = -1;  // Health seen at the end of the last update, -1 until the first update

    // Movement sync
    bool isMovementAuthority = true; // We're the closest player, so our game runs this enemy for everyone
    bool hasTarget = false;          // We've received a position for this enemy from the player running it
    Vec3f targetPos = { 0.0f, 0.0f, 0.0f };
    s16 targetShapeRotY = 0;
    s16 targetWorldRotY = 0;
    u32 targetFrame = 0; // gameplayFrames when the target arrived
};
static ObjectExtension::Register<EnemySyncData> EnemySyncDataRegister;

// key -> silent (true when learned from ENEMY_STATE on scene entry, so the enemy disappears without an effect)
static std::map<std::string, bool> sDefeatedEnemies;
static s16 sEnemySyncSceneNum = -1;
static Vec3f sZeroVec = { 0.0f, 0.0f, 0.0f };

// Movement sync tuning
constexpr u32 ENEMY_MOVEMENT_SEND_INTERVAL = 2;   // Send every 2 game updates (~10 times a second)
constexpr f32 ENEMY_MOVEMENT_AUTHORITY_MARGIN = 50.0f; // Near-ties: both players run the enemy themselves
constexpr u32 ENEMY_MOVEMENT_STALE_FRAMES = 10;   // Stop following a position older than ~half a second
constexpr f32 ENEMY_MOVEMENT_SNAP_DISTANCE = 300.0f; // Further off than this: jump straight there
constexpr f32 ENEMY_MOVEMENT_LERP = 0.5f;          // Otherwise close half the gap each frame
static u32 sEnemyMovementFrameCounter = 0;

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
        case ACTOR_EN_IK:         // Iron Knuckle (Nabooru cutscene)
        case ACTOR_EN_TORCH2:     // Dark Link
        case ACTOR_EN_PO_SISTERS: // Forest Temple Poe sisters
        case ACTOR_EN_PO_FIELD:   // Big Poes (bottle reward)
        case ACTOR_EN_SKJ:        // Skull Kid
        case ACTOR_EN_DNS:        // Business scrubs
        case ACTOR_EN_HINTNUTS:   // Deku Tree 2-3-1 scrub puzzle
            return true;
        case ACTOR_EN_SW: // Gold Skulltulas (token reward). Regular Skullwalltulas are fine.
            return ((actor->params & 0xE000) >> 0xD) != 0;
        default:
            return false;
    }
}

// Enemies whose position must not be driven by another player's game. Wallmasters and flying pots/tiles home in on
// the local player specifically, so pulling them toward someone else's spot would make them miss or float.
bool IsEnemyMovementExcluded(Actor* actor) {
    if (IsEnemySyncExcluded(actor)) {
        return true;
    }

    switch (actor->id) {
        case ACTOR_EN_WALLMAS:   // Wallmaster (drops onto the local player)
        case ACTOR_EN_TUBO_TRAP: // Flying pots
        case ACTOR_EN_YUKABYUN:  // Flying floor tiles
            return true;
        default:
            return false;
    }
}

bool IsGrabbingLocalPlayer(Actor* actor) {
    Player* player = GET_PLAYER(gPlayState);
    return player != nullptr &&
           ((player->stateFlags2 & PLAYER_STATE2_GRABBED_BY_ENEMY) || player->actor.parent == actor ||
            actor->child == &player->actor);
}

// Pull our copy of an enemy toward where the player running it says it is
void ApplyEnemyMovement(Actor* actor, EnemySyncData* data) {
    if (!data->hasTarget || data->isMovementAuthority) {
        return;
    }

    if (gPlayState->gameplayFrames - data->targetFrame > ENEMY_MOVEMENT_STALE_FRAMES) {
        // Nobody has sent this enemy for a while (they left, or we became the closest); let it run on its own
        data->hasTarget = false;
        return;
    }

    // Never drag an enemy that's holding us (Like Like, ReDead, etc.)
    if (IsGrabbingLocalPlayer(actor)) {
        return;
    }

    Vec3f diff;
    f32 dist = Math_Vec3f_DistXYZAndStoreDiff(&actor->world.pos, &data->targetPos, &diff);

    if (dist > ENEMY_MOVEMENT_SNAP_DISTANCE) {
        actor->world.pos = data->targetPos;
    } else {
        actor->world.pos.x += diff.x * ENEMY_MOVEMENT_LERP;
        actor->world.pos.y += diff.y * ENEMY_MOVEMENT_LERP;
        actor->world.pos.z += diff.z * ENEMY_MOVEMENT_LERP;
    }

    actor->shape.rot.y += (s16)((s16)(data->targetShapeRotY - actor->shape.rot.y) * ENEMY_MOVEMENT_LERP);
    actor->world.rot.y += (s16)((s16)(data->targetWorldRotY - actor->world.rot.y) * ENEMY_MOVEMENT_LERP);
}

Actor* FindLiveEnemyByKey(const std::string& key) {
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head; actor != NULL; actor = actor->next) {
        if (actor->update == NULL) {
            continue;
        }

        EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
        if (data != nullptr && !data->handled && data->key == key) {
            return actor;
        }
    }

    return nullptr;
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

    // Watch each enemy's health after it updates. If it dropped, tell everyone else how much.
    COND_HOOK(OnActorUpdate, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (actor->category != ACTORCAT_ENEMY || !roomState.syncEnemies) {
            return;
        }

        EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
        if (data == nullptr || data->handled) {
            return;
        }

        s16 health = actor->colChkInfo.health;
        if (data->lastHealth > health && !IsEnemySyncExcluded(actor)) {
            SendPacket_EnemyDamage(data->key, data->lastHealth - health);
        }
        data->lastHealth = health;

        // Then line our copy up with the player who is running this enemy
        if (roomState.syncEnemyMovement && !IsEnemyMovementExcluded(actor)) {
            ApplyEnemyMovement(actor, data);
        }
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

void Anchor::SendPacket_EnemyDamage(const std::string& key, s16 damage) {
    if (!ShouldSyncEnemies()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_DAMAGE;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = key;
    payload["damage"] = damage;
    payload["quiet"] = true;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_EnemyDamage(nlohmann::json payload) {
    if (!ShouldSyncEnemies()) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    if (sceneNum != gPlayState->sceneNum) {
        return;
    }

    std::string key = payload["key"].get<std::string>();
    s16 damage = payload["damage"].get<s16>();
    if (damage <= 0) {
        return;
    }

    // Only enemies currently loaded in our game take the hit (an enemy in another room of the dungeon is not loaded)
    Actor* actor = FindLiveEnemyByKey(key);
    if (actor == nullptr || IsEnemySyncExcluded(actor)) {
        return;
    }

    s16 health = actor->colChkInfo.health;
    if (health <= 1) {
        return;
    }

    s16 newHealth = health - damage;
    if (newHealth < 1) {
        newHealth = 1;
    }

    actor->colChkInfo.health = (u8)newHealth;

    // Remember the new value so our own health watcher doesn't send this hit back out
    EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
    data->lastHealth = newHealth;

    // Brief red flash so you can see a teammate landed a hit
    Actor_SetColorFilter(actor, 0x4000, 255, 0, 8);
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

// Called every player update. Works out which enemies we're the closest player to, and sends their positions to
// everyone else in the scene.
void Anchor::TickEnemyMovementSync() {
    if (!roomState.syncEnemies || !roomState.syncEnemyMovement || !IsSaveLoaded()) {
        return;
    }

    if (++sEnemyMovementFrameCounter < ENEMY_MOVEMENT_SEND_INTERVAL) {
        return;
    }
    sEnemyMovementFrameCounter = 0;

    // Other players who are standing in this scene with us right now
    std::vector<uint32_t> targets;
    std::vector<Vec3f> otherPositions;
    for (auto& [clientId, client] : clients) {
        if (client.self || !client.online || !client.isSaveLoaded || client.sceneNum != gPlayState->sceneNum ||
            client.player == nullptr) {
            continue;
        }
        targets.push_back(clientId);
        otherPositions.push_back(client.posRot.pos);
    }

    if (targets.empty()) {
        return;
    }

    Player* self = GET_PLAYER(gPlayState);
    nlohmann::json enemies = nlohmann::json::array();

    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head; actor != NULL; actor = actor->next) {
        if (actor->update == NULL || IsEnemyMovementExcluded(actor)) {
            continue;
        }

        EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
        if (data == nullptr || data->handled) {
            continue;
        }

        f32 myDist = Math_Vec3f_DistXYZ(&self->actor.world.pos, &actor->world.pos);
        f32 closestOther = FLT_MAX;
        for (Vec3f& pos : otherPositions) {
            closestOther = std::min(closestOther, Math_Vec3f_DistXYZ(&pos, &actor->world.pos));
        }

        data->isMovementAuthority = myDist <= closestOther + ENEMY_MOVEMENT_AUTHORITY_MARGIN;

        if (data->isMovementAuthority) {
            enemies.push_back({ data->key, lroundf(actor->world.pos.x), lroundf(actor->world.pos.y),
                                lroundf(actor->world.pos.z), actor->shape.rot.y, actor->world.rot.y });
        }
    }

    if (enemies.empty()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_MOVEMENT;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["enemies"] = enemies;
    payload["quiet"] = true;

    for (uint32_t clientId : targets) {
        payload["targetClientId"] = clientId;
        SendJsonToRemote(payload);
    }
}

void Anchor::HandlePacket_EnemyMovement(nlohmann::json payload) {
    if (!ShouldSyncEnemies() || !roomState.syncEnemyMovement) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    if (sceneNum != gPlayState->sceneNum || !payload.contains("enemies")) {
        return;
    }

    for (auto& entry : payload["enemies"]) {
        if (!entry.is_array() || entry.size() < 6) {
            continue;
        }

        Actor* actor = FindLiveEnemyByKey(entry[0].get<std::string>());
        if (actor == nullptr || IsEnemyMovementExcluded(actor)) {
            continue;
        }

        EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
        data->hasTarget = true;
        data->targetPos.x = entry[1].get<f32>();
        data->targetPos.y = entry[2].get<f32>();
        data->targetPos.z = entry[3].get<f32>();
        data->targetShapeRotY = entry[4].get<s16>();
        data->targetWorldRotY = entry[5].get<s16>();
        data->targetFrame = gPlayState->gameplayFrames;
    }
}
