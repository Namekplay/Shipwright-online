#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ObjectExtension.h"

#include <cmath>
#include <deque>
#include <string>
#include <vector>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Kanban/z_en_kanban.h"
#include "src/overlays/actors/ovl_En_Goroiwa/z_en_goroiwa.h"
extern PlayState* gPlayState;

void EnGoroiwa_SetupRoll(EnGoroiwa* thisx);
void EnGoroiwa_Roll(EnGoroiwa* thisx, PlayState* play);
void EnGoroiwa_SetupMoveAndFallToGround(EnGoroiwa* thisx);
void EnGoroiwa_MoveAndFallToGround(EnGoroiwa* thisx, PlayState* play);
void EnGoroiwa_SetupWait(EnGoroiwa* thisx);
void EnGoroiwa_Wait(EnGoroiwa* thisx, PlayState* play);
void EnGoroiwa_SetupMoveUp(EnGoroiwa* thisx);
void EnGoroiwa_MoveUp(EnGoroiwa* thisx, PlayState* play);
void EnGoroiwa_SetupMoveDown(EnGoroiwa* thisx);
void EnGoroiwa_MoveDown(EnGoroiwa* thisx, PlayState* play);
}

/**
 * Prop Sync (signs and rolling boulders)
 *
 * Signs: cutting a sign with your sword splits it differently depending on how you swung and which side you hit it
 * from, and the sign works that out from "the player" - which in every other game is that game's own player. So when
 * you cut a sign, how you swung and where you stood are sent to the other players in the scene, and their copy of the
 * sign is hit once more with exactly that swing and angle, so the same piece flies off for everyone.
 *
 * Rolling boulders (the Kokiri Forest boulder maze, Goron City, Death Mountain Trail...) roll along a set path, but
 * each game starts its boulders the moment that player enters the area, so they end up at different points of their
 * route. One player in the scene (whoever joined the room first) runs the boulders, and a few times a second sends
 * where each one is along its path; everyone else's boulders take on that same state and position.
 *
 * Objects are matched across games the same way as enemies: scene + actor id + params + spawn position.
 *
 * SIGN_CUT       - Broadcast to the room when you cut a sign
 * BOULDER_STATE  - Sent ~5 times a second to the other players in the scene by whoever runs the boulders
 */

namespace {

struct SignCut {
    u8 meleeWeaponAnimation; // How the cutting player swung (decides the cut direction)
    s16 yawDiff;             // Which side of the sign they hit it from, relative to the sign's facing
    u32 dmgFlags;            // What it was hit with (sword or something else)
    uint32_t clientId;       // Who cut it
};

struct PropSyncData {
    std::string key;

    // Signs
    bool cutCandidate = false; // A hit landed on this sign this frame; check after it updates whether it was cut
    bool hitByMe = false;      // ...and the hit was ours
    u16 partFlagsBefore = 0;
    SignCut myCut = {};
    std::deque<SignCut> pendingCuts; // Other players' cuts waiting to be replayed on our copy
    bool restorePlayerAnim = false;
    u8 savedPlayerAnim = 0;

    // Boulders
    u32 lastBoulderStateFrame = 0;
    Vec3f correction = { 0.0f, 0.0f, 0.0f }; // Position error spread over the next few frames
    u8 correctionFramesLeft = 0;
};
static ObjectExtension::Register<PropSyncData> PropSyncDataRegister;

constexpr u32 BOULDER_SEND_INTERVAL = 4;        // ~5 times a second
constexpr f32 BOULDER_SNAP_DISTANCE = 150.0f;   // Further off than this: jump straight there
constexpr u8 BOULDER_CORRECTION_FRAMES = 4;     // Otherwise spread the correction over this many frames
static u32 sBoulderFrameCounter = 0;

// The sign's "standing, in one piece or partly cut" state (first value of its action state list in z_en_kanban.c)
constexpr u8 KANBAN_STATE_SIGN = 0;

// The hit info our replayed sign cuts point at (the sign only reads the weapon flags from it)
static ColliderInfo sSignReplayHitInfo;

enum BoulderAction : u8 {
    BOULDER_ROLL,
    BOULDER_FALL,
    BOULDER_WAIT,
    BOULDER_MOVE_UP,
    BOULDER_MOVE_DOWN,
    BOULDER_ACTION_UNKNOWN,
};

std::string MakePropKey(Actor* actor) {
    return std::to_string(gPlayState->sceneNum) + ":" + std::to_string(actor->id) + ":" +
           std::to_string((u16)actor->params) + ":" + std::to_string(lroundf(actor->home.pos.x)) + ":" +
           std::to_string(lroundf(actor->home.pos.y)) + ":" + std::to_string(lroundf(actor->home.pos.z));
}

bool IsSign(Actor* actor) {
    return actor->id == ACTOR_EN_KANBAN && actor->params != ENKANBAN_PIECE && actor->params != ENKANBAN_FISHING;
}

bool IsBoulder(Actor* actor) {
    return actor->id == ACTOR_EN_GOROIWA;
}

bool ShouldSyncProps() {
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

bool IsOwnedByLocalPlayer(Actor* actor) {
    if (actor == nullptr) {
        return false;
    }

    Actor* self = &GET_PLAYER(gPlayState)->actor;
    return actor == self || actor->parent == self;
}

Actor* FindPropByKey(const std::string& key, bool (*matches)(Actor*)) {
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head; actor != NULL; actor = actor->next) {
        if (actor->update == NULL || !matches(actor)) {
            continue;
        }

        PropSyncData* data = ObjectExtension::GetInstance().Get<PropSyncData>(actor);
        if (data != nullptr && data->key == key) {
            return actor;
        }
    }

    return nullptr;
}

BoulderAction GetBoulderAction(EnGoroiwa* boulder) {
    if (boulder->actionFunc == EnGoroiwa_Roll) {
        return BOULDER_ROLL;
    } else if (boulder->actionFunc == EnGoroiwa_MoveAndFallToGround) {
        return BOULDER_FALL;
    } else if (boulder->actionFunc == EnGoroiwa_Wait) {
        return BOULDER_WAIT;
    } else if (boulder->actionFunc == EnGoroiwa_MoveUp) {
        return BOULDER_MOVE_UP;
    } else if (boulder->actionFunc == EnGoroiwa_MoveDown) {
        return BOULDER_MOVE_DOWN;
    }
    return BOULDER_ACTION_UNKNOWN;
}

void SetBoulderAction(EnGoroiwa* boulder, BoulderAction action) {
    switch (action) {
        case BOULDER_ROLL:
            EnGoroiwa_SetupRoll(boulder);
            break;
        case BOULDER_FALL:
            EnGoroiwa_SetupMoveAndFallToGround(boulder);
            break;
        case BOULDER_WAIT:
            EnGoroiwa_SetupWait(boulder);
            break;
        case BOULDER_MOVE_UP:
            EnGoroiwa_SetupMoveUp(boulder);
            break;
        case BOULDER_MOVE_DOWN:
            EnGoroiwa_SetupMoveDown(boulder);
            break;
        default:
            break;
    }
}

} // namespace

void Anchor::RegisterPropSyncHooks() {
    // Tag signs and boulders with their key before they initialize (home is still the untouched spawn position here)
    COND_HOOK(ShouldActorInit, isConnected, [&](void* refActor, bool* should) {
        Actor* actor = (Actor*)refActor;
        if (gPlayState == nullptr || !(IsSign(actor) || IsBoulder(actor))) {
            return;
        }

        PropSyncData data;
        data.key = MakePropKey(actor);
        ObjectExtension::GetInstance().Set<PropSyncData>(actor, std::move(data));
    });

    // Signs, right before they update
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_KANBAN, isConnected, [&](void* refActor, bool* should) {
        Actor* actor = (Actor*)refActor;
        EnKanban* sign = (EnKanban*)actor;
        if (!IsSign(actor) || !ShouldSyncProps() || sign->actionState != KANBAN_STATE_SIGN ||
            sign->invincibilityTimer != 0) {
            return;
        }

        PropSyncData* data = ObjectExtension::GetInstance().Get<PropSyncData>(actor);
        if (data == nullptr) {
            return;
        }

        Player* self = GET_PLAYER(gPlayState);

        if (sign->collider.base.acFlags & AC_HIT) {
            // Something hit it last frame; the sign is about to process the hit. Note whether it was us, and how.
            data->cutCandidate = true;
            data->partFlagsBefore = sign->partFlags;
            data->hitByMe = IsOwnedByLocalPlayer(sign->collider.base.ac);
            if (data->hitByMe) {
                ColliderInfo* hitInfo = sign->collider.info.acHitInfo;
                data->myCut.meleeWeaponAnimation = self->meleeWeaponAnimation;
                data->myCut.yawDiff = actor->yawTowardsPlayer - actor->shape.rot.y;
                data->myCut.dmgFlags = hitInfo != nullptr ? hitInfo->toucher.dmgFlags : 0;
            }
            return;
        }

        if (data->pendingCuts.empty()) {
            return;
        }

        // Replay another player's cut: hit the sign as if from where they stood, with how they swung. The sign reads
        // the swing from "the player", so lend ours their swing just for this update (put back right after).
        SignCut cut = data->pendingCuts.front();
        data->pendingCuts.pop_front();

        Player* cutter = GetDummyInScene(cut.clientId);
        sSignReplayHitInfo.toucher.dmgFlags = cut.dmgFlags;
        sign->collider.base.acFlags |= AC_HIT;
        sign->collider.base.ac = cutter != nullptr ? &cutter->actor : NULL;
        sign->collider.info.acHitInfo = &sSignReplayHitInfo;
        actor->yawTowardsPlayer = actor->shape.rot.y + cut.yawDiff;

        data->savedPlayerAnim = self->meleeWeaponAnimation;
        data->restorePlayerAnim = true;
        self->meleeWeaponAnimation = cut.meleeWeaponAnimation;
    });

    // Signs, right after they update
    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_KANBAN, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        EnKanban* sign = (EnKanban*)actor;
        if (!IsSign(actor)) {
            return;
        }

        PropSyncData* data = ObjectExtension::GetInstance().Get<PropSyncData>(actor);
        if (data == nullptr) {
            return;
        }

        if (data->restorePlayerAnim) {
            data->restorePlayerAnim = false;
            GET_PLAYER(gPlayState)->meleeWeaponAnimation = data->savedPlayerAnim;
        }

        if (data->cutCandidate) {
            data->cutCandidate = false;
            if (data->hitByMe && sign->partFlags != data->partFlagsBefore) {
                SendPacket_SignCut(data->key, data->myCut.meleeWeaponAnimation, data->myCut.yawDiff,
                                   data->myCut.dmgFlags);
            }
        }
    });

    // Boulders, right after they update: smooth out the last position correction from whoever runs them
    COND_ID_HOOK(OnActorUpdate, ACTOR_EN_GOROIWA, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        PropSyncData* data = ObjectExtension::GetInstance().Get<PropSyncData>(actor);
        if (data == nullptr || data->correctionFramesLeft == 0) {
            return;
        }

        data->correctionFramesLeft--;
        actor->world.pos.x += data->correction.x;
        actor->world.pos.y += data->correction.y;
        actor->world.pos.z += data->correction.z;
    });
}

void Anchor::SendPacket_SignCut(const std::string& key, u8 meleeWeaponAnimation, s16 yawDiff, u32 dmgFlags) {
    if (!ShouldSyncProps()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = SIGN_CUT;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = key;
    payload["anim"] = meleeWeaponAnimation;
    payload["yawDiff"] = yawDiff;
    payload["dmgFlags"] = dmgFlags;
    payload["quiet"] = true;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_SignCut(nlohmann::json payload) {
    if (!ShouldSyncProps() || !payload.contains("clientId")) {
        return;
    }

    if (payload.value("sceneNum", (s16)-1) != gPlayState->sceneNum) {
        return;
    }

    Actor* actor = FindPropByKey(payload.value("key", std::string()), IsSign);
    if (actor == nullptr) {
        return;
    }

    PropSyncData* data = ObjectExtension::GetInstance().Get<PropSyncData>(actor);
    if (data->pendingCuts.size() >= 8) {
        return;
    }

    data->pendingCuts.push_back({ payload.value("anim", (u8)0), payload.value("yawDiff", (s16)0),
                                  payload.value("dmgFlags", (u32)0), payload["clientId"].get<uint32_t>() });
}

// Called every player update. Whoever joined the room first (lowest client id) among the players in this scene runs
// the boulders, and sends their state to everyone else here.
void Anchor::TickBoulderSync() {
    if (!ShouldSyncProps()) {
        return;
    }

    if (++sBoulderFrameCounter < BOULDER_SEND_INTERVAL) {
        return;
    }
    sBoulderFrameCounter = 0;

    std::vector<uint32_t> targets;
    for (auto& [clientId, client] : clients) {
        if (GetDummyInScene(clientId) == nullptr) {
            continue;
        }
        if (clientId < ownClientId) {
            return; // Someone who's been in the room longer is here; they run the boulders
        }
        targets.push_back(clientId);
    }

    if (targets.empty()) {
        return;
    }

    nlohmann::json boulders = nlohmann::json::array();
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head; actor != NULL; actor = actor->next) {
        if (actor->update == NULL || !IsBoulder(actor)) {
            continue;
        }

        PropSyncData* data = ObjectExtension::GetInstance().Get<PropSyncData>(actor);
        EnGoroiwa* boulder = (EnGoroiwa*)actor;
        BoulderAction action = GetBoulderAction(boulder);
        if (data == nullptr || action == BOULDER_ACTION_UNKNOWN) {
            continue;
        }

        boulders.push_back({ data->key, actor->world.pos.x, actor->world.pos.y, actor->world.pos.z, (u8)action,
                             boulder->currentWaypoint, boulder->nextWaypoint, boulder->pathDirection,
                             boulder->waitTimer, actor->speedXZ, actor->velocity.y });
    }

    if (boulders.empty()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = BOULDER_STATE;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["boulders"] = boulders;
    payload["quiet"] = true;

    for (uint32_t clientId : targets) {
        payload["targetClientId"] = clientId;
        SendJsonToRemote(payload);
    }
}

void Anchor::HandlePacket_BoulderState(nlohmann::json payload) {
    if (!ShouldSyncProps() || payload.value("sceneNum", (s16)-1) != gPlayState->sceneNum ||
        !payload.contains("boulders")) {
        return;
    }

    for (auto& entry : payload["boulders"]) {
        if (!entry.is_array() || entry.size() < 11) {
            continue;
        }

        Actor* actor = FindPropByKey(entry[0].get<std::string>(), IsBoulder);
        if (actor == nullptr) {
            continue;
        }

        EnGoroiwa* boulder = (EnGoroiwa*)actor;
        PropSyncData* data = ObjectExtension::GetInstance().Get<PropSyncData>(actor);

        // Same point of the route, same direction, same phase
        BoulderAction action = (BoulderAction)entry[4].get<u8>();
        if (action < BOULDER_ACTION_UNKNOWN && GetBoulderAction(boulder) != action) {
            SetBoulderAction(boulder, action);
        }
        boulder->currentWaypoint = entry[5].get<s16>();
        boulder->nextWaypoint = entry[6].get<s16>();
        boulder->pathDirection = entry[7].get<s16>();
        boulder->waitTimer = entry[8].get<s16>();
        actor->speedXZ = entry[9].get<f32>();
        actor->velocity.y = entry[10].get<f32>();

        // Same place: jump there if far off, otherwise ease in over a few frames
        Vec3f target = { entry[1].get<f32>(), entry[2].get<f32>(), entry[3].get<f32>() };
        Vec3f diff;
        f32 dist = Math_Vec3f_DistXYZAndStoreDiff(&actor->world.pos, &target, &diff);
        if (dist > BOULDER_SNAP_DISTANCE) {
            actor->world.pos = target;
            data->correctionFramesLeft = 0;
        } else {
            data->correction.x = diff.x / BOULDER_CORRECTION_FRAMES;
            data->correction.y = diff.y / BOULDER_CORRECTION_FRAMES;
            data->correction.z = diff.z / BOULDER_CORRECTION_FRAMES;
            data->correctionFramesLeft = BOULDER_CORRECTION_FRAMES;
        }
    }
}
