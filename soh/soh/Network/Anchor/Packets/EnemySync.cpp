#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/OTRGlobals.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <string>
#include <vector>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * Enemy Sync
 *
 * Every player's game still runs its own copy of each enemy. These pieces keep the copies matching:
 *
 * Perception: an enemy reacts to whichever player is closest to it, not only to you. A Deku Baba pops up on your
 * screen when your friend walks up to it, a Stalfos turns to face whoever it is fighting, and so on. While the enemy
 * runs its update, the closest player's character *is* "the player" as far as that enemy can tell, so every check it
 * makes (distances, directions, where the player is standing, what they're doing) agrees. Earlier builds only changed
 * the distance/direction values, so enemies that also look at the player's position directly (Deku Babas) got mixed
 * answers and twitched between growing and hiding. (Enemies that grab or freeze the player - Like Like, ReDead,
 * Wallmaster, Floormaster, Dead Hand - only react to you, otherwise they could grab you from across the room.)
 *
 * Movement: for each enemy, the player standing closest to it is in charge of it. That player's game runs the enemy
 * normally and sends where it is and which way it faces ~10 times a second. Everyone else's copy is pulled toward that
 * position each frame (or snapped there if it's far off). Control passes automatically when someone else gets closer.
 *
 * Hits: when you hit an enemy, the hit itself (what weapon, how hard) is sent to everyone else, and their game replays
 * it on their copy of the enemy as a real hit from your character. So they see the same flinch, knockback, stun or
 * burn, and when the hit is the killing blow they see the enemy's own death animation.
 *
 * Kills: when an enemy dies in someone's game, it's marked dead for everyone in the scene. If it's still alive in your
 * game (a replayed hit missed because it was in a slightly different pose), the killing blow is replayed on it with
 * its health set to 1, so it still dies its normal death. Only if that fails does it vanish in a blue flame.
 * Players entering a scene later are told which enemies are already dead there and those don't spawn.
 *
 * Enemies are identified across games by: scene + actor id + params + spawn (home) position. Enemies placed in the
 * scene/room data spawn at the exact same position for everyone, so this key matches between games. Enemies spawned
 * randomly at runtime (Hyrule Field Stalchildren, Leevers, etc.) won't match and are simply left alone.
 *
 * The list of dead enemies lasts while you stay in the scene (moving between rooms of a dungeon included) and is
 * cleared when you go to a different scene, so enemies respawn the same way they normally would.
 *
 * ENEMY_HIT           - Broadcast to the room when you land a hit on an enemy
 * ENEMY_DEFEATED      - Broadcast to the room when an enemy dies in your game
 * REQUEST_ENEMY_STATE - Broadcast to the room when you enter a scene, asking for that scene's dead enemies
 * ENEMY_STATE         - Reply sent directly to the player who asked
 * ENEMY_MOVEMENT      - Sent ~10 times a second to players in the same scene with the enemies you're in charge of
 */

namespace {

struct PendingHit {
    u32 dmgFlags;
    u8 damage;
    uint32_t clientId;
};

struct EnemySyncData {
    std::string key;
    bool handled = false; // Already defeated in this game (by us or by a sync), don't touch it again

    // Hits
    u32 lastHitSentFrame = 0;            // Only send one hit per enemy per frame (multi-part colliders report several)
    u32 lastDmgFlags = 0;                // What we last hit it with, sent along with ENEMY_DEFEATED
    std::vector<PendingHit> pendingHits; // Other players' hits waiting to be replayed on our copy
    u32 killStartFrame = 0;              // When we started replaying someone else's killing blow, 0 = not started
    bool lastHitWasReplay = false;       // The last hit it took in our game was another player's hit being replayed

    // Movement sync
    bool isMovementAuthority = true; // We're the closest player, so our game runs this enemy for everyone
    bool hasTarget = false;          // We've received a position for this enemy from the player running it
    Vec3f targetPos = { 0.0f, 0.0f, 0.0f };
    s16 targetShapeRotY = 0;
    s16 targetWorldRotY = 0;
    u32 targetFrame = 0; // gameplayFrames when the target arrived
};
static ObjectExtension::Register<EnemySyncData> EnemySyncDataRegister;

struct DefeatInfo {
    bool silent = false;   // Died before we arrived in the scene: just don't be there, no death at all
    u32 dmgFlags = 0;      // What the killing blow was, to replay it
    uint32_t clientId = 0; // Who landed it
};

static std::map<std::string, DefeatInfo> sDefeatedEnemies;
static s16 sEnemySyncSceneNum = -1;
static Vec3f sZeroVec = { 0.0f, 0.0f, 0.0f };

// Movement sync tuning
constexpr u32 ENEMY_MOVEMENT_SEND_INTERVAL = 2;        // Send every 2 game updates (~10 times a second)
constexpr f32 ENEMY_MOVEMENT_AUTHORITY_MARGIN = 50.0f; // Near-ties: both players run the enemy themselves
constexpr u32 ENEMY_MOVEMENT_STALE_FRAMES = 10;        // Stop following a position older than ~half a second
constexpr f32 ENEMY_MOVEMENT_SNAP_DISTANCE = 300.0f;   // Further off than this: jump straight there
constexpr f32 ENEMY_MOVEMENT_LERP = 0.5f;              // Otherwise close half the gap each frame
static u32 sEnemyMovementFrameCounter = 0;

// Killing blow replay tuning (in game updates, ~20 per second)
constexpr u32 KILL_RETRY_INTERVAL = 10;  // Re-send the killing blow every half second while it's still alive
constexpr u32 KILL_FALLBACK_FRAMES = 40; // Still alive after 2 seconds: give up and use the blue flame
constexpr u32 KILL_GIVE_UP_FRAMES = 200; // Stuck mid-death for 10 seconds: just remove it
constexpr size_t MAX_PENDING_HITS = 4;

// Replayed hits are real attack colliders owned by the other player's character. The enemy reads the collider back
// the frame after it's hit, so each one has to stay alive for a couple of frames; a small ring of them is plenty.
constexpr size_t REPLAY_COLLIDER_COUNT = 16;
static ColliderCylinder sReplayColliders[REPLAY_COLLIDER_COUNT];
static size_t sReplayColliderNext = 0;
static ColliderCylinderInit sReplayColliderInit = {
    {
        COLTYPE_NONE,
        AT_ON | AT_TYPE_PLAYER,
        AC_NONE,
        OC1_NONE,
        OC2_TYPE_PLAYER,
        COLSHAPE_CYLINDER,
    },
    {
        ELEMTYPE_UNK2,
        { DMG_SLASH_MASTER, 0x00, 0x02 },
        { 0x00000000, 0x00, 0x00 },
        TOUCH_ON | TOUCH_SFX_NORMAL,
        BUMP_NONE,
        OCELEM_NONE,
    },
    { 35, 70, -35, { 0, 0, 0 } },
};

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

// Enemies that home in on the local player specifically (Wallmasters, flying pots and floor tiles). Pulling them
// toward someone else's spot, or letting them notice someone else, would make them miss or float.
bool IsLocalPlayerHomingEnemy(Actor* actor) {
    switch (actor->id) {
        case ACTOR_EN_WALLMAS:   // Wallmaster (drops onto the local player)
        case ACTOR_EN_TUBO_TRAP: // Flying pots
        case ACTOR_EN_YUKABYUN:  // Flying floor tiles
            return true;
        default:
            return false;
    }
}

// Enemies whose position must not be driven by another player's game
bool IsEnemyMovementExcluded(Actor* actor) {
    if (IsEnemySyncExcluded(actor) || IsLocalPlayerHomingEnemy(actor)) {
        return true;
    }

    switch (actor->id) {
        // Rooted or wall-mounted enemies. They never go anywhere, but their position is used to animate them (a Deku
        // Baba's position is its head on the end of its stem), so pulling it toward another game's copy tears the
        // head off the stem. Their attacks still react to whichever player is closest.
        case ACTOR_EN_DEKUBABA: // Deku Baba
        case ACTOR_EN_KAREBABA: // Big / withered Deku Baba
        case ACTOR_EN_DEKUNUTS: // Mad Scrub
        case ACTOR_EN_OKUTA:    // Octorok
        case ACTOR_EN_ST:       // Skulltula (hangs from its thread)
        case ACTOR_EN_SW:       // Skullwalltula (on walls)
        case ACTOR_EN_VM:       // Beamos
        case ACTOR_EN_BA:       // Jabu-Jabu tentacles
        case ACTOR_EN_DHA:      // Dead Hand's hands
            return true;
        default:
            return false;
    }
}

// Enemies that must only ever notice the local player. These grab, swallow or freeze "the player" as soon as they
// think the player is close, and the player they act on is always you - so if they noticed a friend standing next to
// them, they would grab or freeze you from wherever you are.
bool IsEnemyPerceptionExcluded(Actor* actor) {
    if (IsEnemySyncExcluded(actor) || IsLocalPlayerHomingEnemy(actor)) {
        return true;
    }

    switch (actor->id) {
        case ACTOR_EN_RR:       // Like Like
        case ACTOR_EN_RD:       // ReDead / Gibdo (scream freezes the player)
        case ACTOR_EN_FLOORMAS: // Floormaster
        case ACTOR_EN_DH:       // Dead Hand
        case ACTOR_EN_DHA:      // Dead Hand's hands
            return true;
        default:
            return false;
    }
}

bool IsDummyPlayer(Actor* actor) {
    return actor != nullptr && actor->id == ACTOR_EN_OE2 && actor->update == DummyPlayer_Update;
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

bool IsGrabbingLocalPlayer(Actor* actor) {
    Player* player = GET_PLAYER(gPlayState);
    return player != nullptr &&
           ((player->stateFlags2 & PLAYER_STATE2_GRABBED_BY_ENEMY) || player->actor.parent == actor ||
            actor->child == &player->actor);
}

bool ShouldSyncEnemies() {
    return Anchor::Instance != nullptr && Anchor::Instance->roomState.syncEnemies && Anchor::Instance->IsSaveLoaded();
}

bool ShouldSyncEnemyMovement() {
    return ShouldSyncEnemies() && Anchor::Instance->roomState.syncEnemyMovement;
}

// Make the enemy react to whichever player is closest: point its "distance/direction to the player" values at that
// player, and return that player's character so it can stand in as "the player" while the enemy updates.
// Returns nullptr when we're the closest (nothing to change).
Actor* ApplyNearestPlayerPerception(Actor* actor) {
    Actor* nearest = nullptr;
    f32 nearestDistSq = actor->xyzDistToPlayerSq;

    for (auto& [clientId, client] : Anchor::Instance->clients) {
        Player* dummy = GetDummyInScene(clientId);
        if (dummy == nullptr) {
            continue;
        }

        f32 xz = Actor_WorldDistXZToActor(actor, &dummy->actor);
        f32 y = Actor_HeightDiff(actor, &dummy->actor);
        f32 distSq = SQ(xz) + SQ(y);
        if (distSq < nearestDistSq) {
            nearestDistSq = distSq;
            nearest = &dummy->actor;
        }
    }

    if (nearest == nullptr) {
        return nullptr; // We're the closest; the game already set everything up for us
    }

    actor->xzDistToPlayer = Actor_WorldDistXZToActor(actor, nearest);
    actor->yDistToPlayer = Actor_HeightDiff(actor, nearest);
    actor->xyzDistToPlayerSq = nearestDistSq;
    actor->yawTowardsPlayer = Actor_WorldYawTowardActor(actor, nearest);
    return nearest;
}

// Replay another player's hit on our copy of the enemy: a real attack, from their character, at the enemy
void FireReplayHit(Actor* actor, const PendingHit& hit) {
    Player* attacker = GetDummyInScene(hit.clientId);
    if (attacker == nullptr) {
        return;
    }

    ColliderCylinder* collider = &sReplayColliders[sReplayColliderNext];
    sReplayColliderNext = (sReplayColliderNext + 1) % REPLAY_COLLIDER_COUNT;

    Collider_InitCylinder(gPlayState, collider);
    Collider_SetCylinder(gPlayState, collider, &attacker->actor, &sReplayColliderInit);
    collider->info.toucher.dmgFlags = hit.dmgFlags != 0 ? hit.dmgFlags : DMG_SLASH_MASTER;
    collider->info.toucher.damage = hit.damage;
    collider->dim.pos.x = (s16)actor->focus.pos.x;
    collider->dim.pos.y = (s16)actor->focus.pos.y;
    collider->dim.pos.z = (s16)actor->focus.pos.z;

    CollisionCheck_SetAT(gPlayState, &gPlayState->colChkCtx, &collider->base);
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

} // namespace

// True when an enemy dying right now in our game is really another player's kill (their hit replayed here, or their
// killing blow being replayed). Their game drops the items for that kill and shares them, so ours shouldn't.
bool AnchorEnemySync_IsOtherPlayersKill(Actor* enemy) {
    if (enemy == nullptr || enemy->category != ACTORCAT_ENEMY || Anchor::Instance == nullptr ||
        !Anchor::Instance->roomState.syncEnemies) {
        return false;
    }

    EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(enemy);
    if (data == nullptr) {
        return false;
    }

    if (data->killStartFrame != 0 || data->lastHitWasReplay) {
        return true;
    }

    auto it = sDefeatedEnemies.find(data->key);
    return it != sDefeatedEnemies.end() && it->second.clientId != Anchor::Instance->ownClientId;
}

bool AnchorEnemySync_WillReplayHit(Actor* victim) {
    if (victim == nullptr || victim->category != ACTORCAT_ENEMY || !ShouldSyncEnemies() ||
        IsEnemySyncExcluded(victim)) {
        return false;
    }

    EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(victim);
    return data != nullptr && !data->handled;
}

void Anchor::RegisterEnemySyncHooks() {
    // Tag every enemy with its key before it initializes (home position is still the untouched spawn position here).
    // If someone already defeated it, don't let it spawn at all.
    COND_HOOK(ShouldActorInit, isConnected, [&](void* actorRef, bool* should) {
        Actor* actor = (Actor*)actorRef;
        // Tag everything except players, not only actors that start out as enemies: several enemies spawn as another
        // kind of actor and only become enemies during their own init (Skullwalltulas spawn as NPCs, some Stalfos,
        // Big Deku Babas, Leevers and Big Octo switch over too). The tag is only ever used once they're enemies.
        if (gPlayState == nullptr || actor->category == ACTORCAT_PLAYER) {
            return;
        }

        EnemySyncData data;
        data.key = MakeEnemyKey(actor);
        data.handled = roomState.syncEnemies && !IsEnemySyncExcluded(actor) && sDefeatedEnemies.contains(data.key);
        bool alreadyDefeated = data.handled;
        ObjectExtension::GetInstance().Set<EnemySyncData>(actor, std::move(data));

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

    // An enemy died in our game (to our hit or a replayed one): remember it and tell everyone else
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
        data->pendingHits.clear();
        sDefeatedEnemies.emplace(data->key, DefeatInfo{ false, data->lastDmgFlags, ownClientId });
        SendPacket_EnemyDefeated(data->key, data->lastDmgFlags);
    });

    // A hit landed on an enemy. If we (our sword, arrows, bombs...) landed it, send it to everyone so they can replay
    // it. Hits from other players' characters are the replays themselves, so those are never sent back out.
    COND_HOOK(OnCollisionDamage, isConnected,
              [&](void* refVictim, void* refAttacker, uint32_t dmgFlags, uint8_t damage) {
                  Actor* victim = (Actor*)refVictim;
                  Actor* attacker = (Actor*)refAttacker;
                  if (!ShouldSyncEnemies() || victim == nullptr || victim->category != ACTORCAT_ENEMY ||
                      attacker == nullptr || attacker->category == ACTORCAT_ENEMY ||
                      attacker->category == ACTORCAT_BOSS || IsEnemySyncExcluded(victim)) {
                      return;
                  }

                  EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(victim);
                  if (data == nullptr || data->handled) {
                      return;
                  }

                  // Remember whose hit this was, so a kill from a replayed hit doesn't drop items a second time
                  data->lastHitWasReplay = IsDummyPlayer(attacker);
                  if (data->lastHitWasReplay || data->lastHitSentFrame == gPlayState->gameplayFrames) {
                      return;
                  }

                  data->lastHitSentFrame = gPlayState->gameplayFrames;
                  data->lastDmgFlags = dmgFlags;
                  SendPacket_EnemyHit(data->key, dmgFlags, damage);
              });

    // Right as an enemy updates: let it notice whichever player is closest, with that player's character standing in
    // as "the player" for this one update (the game puts the real player back as soon as the update is done)
    COND_HOOK(OnActorUpdateBegin, isConnected, [&](void* refActor, void** playerOverride) {
        Actor* actor = (Actor*)refActor;
        if (actor->category != ACTORCAT_ENEMY || actor->update == NULL || !ShouldSyncEnemyMovement() ||
            IsEnemyPerceptionExcluded(actor)) {
            return;
        }

        EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
        if (data == nullptr || data->handled) {
            return;
        }

        Actor* nearest = ApplyNearestPlayerPerception(actor);
        if (nearest != nullptr) {
            *playerOverride = nearest;
        }
    });

    // After an enemy updates: replay other players' hits on it, then line it up with whoever is running it
    COND_HOOK(OnActorUpdate, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (actor->category != ACTORCAT_ENEMY || !roomState.syncEnemies) {
            return;
        }

        EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
        if (data == nullptr || data->handled || IsEnemySyncExcluded(actor)) {
            return;
        }

        if (!data->pendingHits.empty()) {
            FireReplayHit(actor, data->pendingHits.front());
            data->pendingHits.erase(data->pendingHits.begin());
        }

        if (roomState.syncEnemyMovement && !IsEnemyMovementExcluded(actor)) {
            ApplyEnemyMovement(actor, data);
        }
    });

    // Enemies that someone else killed: replay the killing blow so it dies its own death here too
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
        if (IsGrabbingLocalPlayer(actor)) {
            return;
        }

        const DefeatInfo& info = it->second;
        u32 now = gPlayState->gameplayFrames;

        // Died before we arrived: it just isn't here
        if (info.silent) {
            data->handled = true;
            Actor_Kill(actor);
            *should = false;
            return;
        }

        bool canReplay = GetDummyInScene(info.clientId) != nullptr;

        if (data->killStartFrame == 0 && canReplay) {
            // First time: drop it to 1 health and replay the killing blow, then let it update normally
            data->killStartFrame = now == 0 ? 1 : now;
            if (actor->colChkInfo.health > 1) {
                actor->colChkInfo.health = 1;
            }
            data->pendingHits.clear();
            data->pendingHits.push_back({ info.dmgFlags, 2, info.clientId });
            return;
        }

        u32 elapsed = data->killStartFrame == 0 ? KILL_GIVE_UP_FRAMES : now - data->killStartFrame;

        if (actor->colChkInfo.health == 0) {
            // It's playing its death animation. Let it finish, unless it's somehow stuck.
            if (elapsed < KILL_GIVE_UP_FRAMES) {
                return;
            }
            data->handled = true;
            Actor_Kill(actor);
            *should = false;
            return;
        }

        if (canReplay && elapsed < KILL_FALLBACK_FRAMES) {
            // Still alive: the replayed blow may have missed or bounced. Try again every so often.
            if (elapsed % KILL_RETRY_INTERVAL == 0 && data->pendingHits.empty()) {
                data->pendingHits.push_back({ info.dmgFlags, 2, info.clientId });
            }
            return;
        }

        // The killing blow couldn't land (immune to that weapon, the killer left, etc.): blue flame as a last resort
        data->handled = true;
        EffectSsDeadDb_Spawn(gPlayState, &actor->world.pos, &sZeroVec, &sZeroVec, 100, 0, 255, 255, 255, 255, 0, 0,
                             255, 1, 9, true);
        Actor_Kill(actor);
        *should = false;
    });
}

void Anchor::SendPacket_EnemyHit(const std::string& key, u32 dmgFlags, u8 damage) {
    if (!ShouldSyncEnemies()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_HIT;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = key;
    payload["dmgFlags"] = dmgFlags;
    payload["damage"] = damage;
    payload["quiet"] = true;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_EnemyHit(nlohmann::json payload) {
    if (!ShouldSyncEnemies() || !payload.contains("clientId")) {
        return;
    }

    s16 sceneNum = payload["sceneNum"].get<s16>();
    if (sceneNum != gPlayState->sceneNum) {
        return;
    }

    // Only enemies currently loaded in our game take the hit (an enemy in another room of the dungeon is not loaded)
    Actor* actor = FindLiveEnemyByKey(payload["key"].get<std::string>());
    if (actor == nullptr || IsEnemySyncExcluded(actor)) {
        return;
    }

    EnemySyncData* data = ObjectExtension::GetInstance().Get<EnemySyncData>(actor);
    if (data->pendingHits.size() >= MAX_PENDING_HITS) {
        return;
    }

    data->pendingHits.push_back(
        { payload["dmgFlags"].get<u32>(), payload["damage"].get<u8>(), payload["clientId"].get<uint32_t>() });
}

void Anchor::SendPacket_EnemyDefeated(const std::string& key, u32 dmgFlags) {
    if (!ShouldSyncEnemies()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_DEFEATED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = key;
    payload["dmgFlags"] = dmgFlags;

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

    sDefeatedEnemies.emplace(
        key, DefeatInfo{ false, payload.value("dmgFlags", (u32)0), payload.value("clientId", (uint32_t)0) });
}

// Older builds of this mod sent health drops instead of hits. Nothing sends this any more; kept so a stray packet
// from an old build is simply ignored.
void Anchor::HandlePacket_EnemyDamage(nlohmann::json payload) {
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
    for (auto& [key, info] : sDefeatedEnemies) {
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
            sDefeatedEnemies.emplace(keyStr, DefeatInfo{ true, 0, 0 });
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
        if (GetDummyInScene(clientId) == nullptr) {
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
    if (!ShouldSyncEnemyMovement()) {
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
