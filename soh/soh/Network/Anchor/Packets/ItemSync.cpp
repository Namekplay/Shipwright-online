#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ObjectExtension.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Boom/z_en_boom.h"
#include "src/overlays/actors/ovl_En_Bom/z_en_bom.h"
#include "src/overlays/actors/ovl_En_Bom_Chu/z_en_bom_chu.h"
#include "src/overlays/actors/ovl_Arms_Hook/z_arms_hook.h"
#include "src/overlays/actors/ovl_En_Bombf/z_en_bombf.h"
extern PlayState* gPlayState;

std::string AnchorEnemySync_GetKey(Actor* actor);

void ArmsHook_Draw(Actor* thisx, PlayState* play);
void ArmsHook_Wait(ArmsHook* this_, PlayState* play);
void ArmsHook_Shoot(ArmsHook* this_, PlayState* play);
}

/**
 * Item Sync (boomerang, bombs, bombchus, hookshot, bomb flowers)
 *
 * These only exist in the game of the player using them, so on everyone else's screen a friend throwing a boomerang,
 * lighting a bomb or firing the hookshot looked like nothing happened. They don't fly in a straight line on their own
 * like seeds and arrows (see ProjectileSync): a boomerang curves back to whoever threw it, a bomb is carried, thrown,
 * rolls and blows up, a bombchu drives along walls, and the hookshot's chain is attached to the player's hand.
 *
 * So instead of launching a copy and letting it go, every PLAYER_UPDATE (sent every frame to players in the same
 * scene) carries where each of your items is right now. Everyone else's game shows a copy of each one, placed exactly
 * there every frame:
 *   - The boomerang spins and leaves its trail.
 *   - Bombs fizz, flash red and blow up (with the flash, smoke, shockwave and sound) at the same moment yours does.
 *     Bombchus blink and blow up too (their explosion is a bomb, which comes through the same way).
 *   - The hookshot's tip and chain are drawn from the other player's character's hand.
 *
 * Copies can't hit, push or be picked up by anything: your item's real hits are already sent separately (see
 * EnemySync), so a copy hitting things too would count every hit twice.
 *
 * Bomb flowers: pulling the bomb off a flower (picking it up, hitting it, lighting it) empties that flower in everyone's
 * game, and it grows back on the same timer. The bomb you pulled off is one of your items like any other bomb, so
 * everyone sees you carry it, throw it and see it blow up.
 *
 * BOMB_FLOWER_PLUCKED - Broadcast to the room when a bomb comes off a bomb flower in your game
 */

namespace {

struct ItemSyncData {
    // Our own item
    bool mine = false;
    uint32_t localId = 0;

    // A copy of another player's item, and where/how their game says it is right now
    bool copy = false;
    uint32_t ownerClientId = 0;
    uint32_t ownerItemId = 0;
    Vec3f pos = { 0.0f, 0.0f, 0.0f };
    Vec3s rot = { 0, 0, 0};
    s16 extraA = 0; // Boomerang: world.rot.x   Bombs/bombchu: fuse timer   Hookshot: 1 while shooting
    s16 extraB = 0; // Boomerang: world.rot.y   Bombs: body (0) or exploding (1)   Hookshot: shot timer
};
static ObjectExtension::Register<ItemSyncData> ItemSyncDataRegister;

struct BombFlowerData {
    bool hadBomb = false; // The flower had a full-grown bomb on it when its update started
};
static ObjectExtension::Register<BombFlowerData> BombFlowerDataRegister;


static uint32_t sNextItemId = 1;
static bool sSpawningCopy = false;
static s16 sItemSyncScene = -1;

// Copies that finished on their own (a bomb that blew up): don't bring them back while the owner's is still fading
static std::map<uint32_t, std::set<uint32_t>> sFinishedCopies;

// Bomb flower bombs (ACTOR_EN_BOMBF) count, but never the flowers themselves: those only ever get ItemSyncData when
// they're a bomb that came off a flower
bool IsSyncedItem(s16 actorId) {
    return actorId == ACTOR_EN_BOOM || actorId == ACTOR_EN_BOM || actorId == ACTOR_EN_BOM_CHU ||
           actorId == ACTOR_ARMS_HOOK || actorId == ACTOR_EN_BOMBF;
}

bool IsBomb(s16 actorId) {
    return actorId == ACTOR_EN_BOM || actorId == ACTOR_EN_BOMBF;
}

bool IsBombFlower(Actor* actor) {
    return actor != nullptr && actor->id == ACTOR_EN_BOMBF && actor->params == BOMBFLOWER_FLOWER;
}

ItemSyncData* GetItemData(Actor* actor) {
    if (actor == nullptr || !IsSyncedItem(actor->id)) {
        return nullptr;
    }
    return ObjectExtension::GetInstance().Get<ItemSyncData>(actor);
}

bool IsMine(Actor* actor) {
    ItemSyncData* data = GetItemData(actor);
    return data != nullptr && data->mine;
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

template <typename F> void ForEachSyncedItem(F&& func) {
    static const u8 categories[] = { ACTORCAT_EXPLOSIVE, ACTORCAT_ITEMACTION, ACTORCAT_MISC };
    for (u8 category : categories) {
        Actor* actor = gPlayState->actorCtx.actorLists[category].head;
        while (actor != NULL) {
            Actor* next = actor->next; // func may kill it
            if (IsSyncedItem(actor->id)) {
                func(actor);
            }
            actor = next;
        }
    }
}

// A copy can't touch anything: no attack, can't be hit, doesn't push or get pushed
void MakeHarmless(Actor* actor) {
    switch (actor->id) {
        case ACTOR_EN_BOOM:
            ((EnBoom*)actor)->collider.base.atFlags &= ~(AT_ON | AT_HIT);
            break;
        case ACTOR_ARMS_HOOK:
            ((ArmsHook*)actor)->collider.base.atFlags &= ~(AT_ON | AT_HIT);
            break;
        case ACTOR_EN_BOM_CHU: {
            EnBomChu* chu = (EnBomChu*)actor;
            chu->collider.base.atFlags &= ~(AT_ON | AT_HIT);
            chu->collider.base.acFlags &= ~(AC_ON | AC_HIT);
            chu->collider.base.ocFlags1 &= ~(OC1_ON | OC1_HIT);
            break;
        }
        case ACTOR_EN_BOM: {
            EnBom* bomb = (EnBom*)actor;
            bomb->bombCollider.base.acFlags &= ~(AC_ON | AC_HIT);
            bomb->bombCollider.base.ocFlags1 &= ~(OC1_ON | OC1_HIT);
            bomb->explosionCollider.base.atFlags &= ~(AT_ON | AT_HIT);
            break;
        }
        case ACTOR_EN_BOMBF: {
            EnBombf* bomb = (EnBombf*)actor;
            bomb->bombCollider.base.acFlags &= ~(AC_ON | AC_HIT);
            bomb->bombCollider.base.ocFlags1 &= ~(OC1_ON | OC1_HIT);
            bomb->explosionCollider.base.atFlags &= ~(AT_ON | AT_HIT);
            break;
        }
    }
}

// The hookshot draws its chain from "the player's" hand: for a copy, that's the other player's character
void ArmsHookCopy_Draw(Actor* actor, PlayState* play) {
    ItemSyncData* data = GetItemData(actor);
    Player* owner = data != nullptr ? GetDummyInScene(data->ownerClientId) : nullptr;
    if (owner == nullptr) {
        return;
    }

    Actor* realPlayer = play->actorCtx.actorLists[ACTORCAT_PLAYER].head;
    play->actorCtx.actorLists[ACTORCAT_PLAYER].head = &owner->actor;
    ArmsHook_Draw(actor, play);
    play->actorCtx.actorLists[ACTORCAT_PLAYER].head = realPlayer;
}

Actor* FindCopy(uint32_t ownerClientId, uint32_t ownerItemId) {
    Actor* found = nullptr;
    ForEachSyncedItem([&](Actor* actor) {
        ItemSyncData* data = GetItemData(actor);
        if (found == nullptr && actor->update != NULL && data != nullptr && data->copy &&
            data->ownerClientId == ownerClientId && data->ownerItemId == ownerItemId) {
            found = actor;
        }
    });
    return found;
}

void KillCopiesOf(uint32_t ownerClientId) {
    ForEachSyncedItem([&](Actor* actor) {
        ItemSyncData* data = GetItemData(actor);
        if (actor->update != NULL && data != nullptr && data->copy && data->ownerClientId == ownerClientId) {
            Actor_Kill(actor);
        }
    });
}

// The fuse timer a copied bomb should have before its update runs (its update counts it down by one)
void SetCopyBombTimer(Actor* actor, ItemSyncData* data) {
    if (actor->params != BOMB_BODY) {
        return; // Already blowing up here: let it finish
    }

    s16 timer;
    if (data->extraB == BOMB_EXPLOSION || data->extraA <= 0) {
        timer = 1; // Theirs went off (maybe early, hit by something): ours goes off now
    } else {
        timer = data->extraA + 1;
    }

    if (actor->id == ACTOR_EN_BOM) {
        ((EnBom*)actor)->timer = timer;
    } else {
        EnBombf* bomb = (EnBombf*)actor;
        bomb->timer = timer;
        bomb->isFuseEnabled = 1;
    }
}

Actor* SpawnCopy(uint32_t ownerClientId, uint32_t ownerItemId, s16 actorId, ItemSyncData& state) {
    s16 params = 0;
    if (actorId == ACTOR_EN_BOM) {
        params = BOMB_BODY;
    } else if (actorId == ACTOR_EN_BOMBF) {
        params = BOMBFLOWER_BODY; // The bomb, not a flower
    }

    sSpawningCopy = true;
    Actor* actor = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId, state.pos.x, state.pos.y, state.pos.z,
                               state.rot.x, state.rot.y, actorId == ACTOR_EN_BOM ? 0 : state.rot.z, params);
    sSpawningCopy = false;

    if (actor == nullptr) {
        return nullptr;
    }

    ItemSyncData data = state;
    data.mine = false;
    data.copy = true;
    data.ownerClientId = ownerClientId;
    data.ownerItemId = ownerItemId;
    ObjectExtension::GetInstance().Set<ItemSyncData>(actor, std::move(data));

    actor->room = -1; // Follows the player around, like the real one
    MakeHarmless(actor);

    if (actorId == ACTOR_ARMS_HOOK) {
        actor->draw = ArmsHookCopy_Draw;
    } else if (actorId == ACTOR_EN_BOM) {
        EnBom* bomb = (EnBom*)actor;
        // A bomb starts tiny and pops to full size a few frames after it's pulled out; a copy of one that's already
        // out shows up full size
        if (state.extraA <= 67 || state.extraB == BOMB_EXPLOSION) {
            Actor_SetScale(actor, 0.01f);
        }
        actor->gravity = 0.0f;
        bomb->timer = std::max<s16>(state.extraA, 1);
    } else if (actorId == ACTOR_EN_BOMBF) {
        actor->gravity = 0.0f;
        ((EnBombf*)actor)->timer = std::max<s16>(state.extraA, 1);
    }

    return actor;
}

void PlaceCopy(Actor* actor, ItemSyncData* data) {
    actor->world.pos = data->pos;
    actor->shape.rot = data->rot;
    actor->velocity.x = actor->velocity.y = actor->velocity.z = 0.0f;
    actor->speedXZ = 0.0f;
}

} // namespace

// Called while building our PLAYER_UPDATE: where each of our items is right now
nlohmann::json AnchorItemSync_Collect() {
    nlohmann::json items = nlohmann::json::array();
    if (gPlayState == nullptr) {
        return items;
    }

    ForEachSyncedItem([&](Actor* actor) {
        ItemSyncData* data = GetItemData(actor);
        if (actor->update == NULL || data == nullptr || !data->mine) {
            return;
        }

        s16 extraA = 0;
        s16 extraB = 0;
        switch (actor->id) {
            case ACTOR_EN_BOOM:
                extraA = actor->world.rot.x;
                extraB = actor->world.rot.y;
                break;
            case ACTOR_EN_BOM:
                extraA = ((EnBom*)actor)->timer;
                extraB = actor->params;
                break;
            case ACTOR_EN_BOMBF:
                extraA = ((EnBombf*)actor)->timer;
                extraB = actor->params;
                break;
            case ACTOR_EN_BOM_CHU:
                extraA = ((EnBomChu*)actor)->timer;
                break;
            case ACTOR_ARMS_HOOK: {
                ArmsHook* hook = (ArmsHook*)actor;
                extraA = hook->actionFunc == ArmsHook_Shoot ? 1 : 0;
                extraB = hook->timer;
                break;
            }
        }

        items.push_back({ data->localId, actor->id, lroundf(actor->world.pos.x), lroundf(actor->world.pos.y),
                          lroundf(actor->world.pos.z), actor->shape.rot.x, actor->shape.rot.y, actor->shape.rot.z,
                          extraA, extraB });
    });

    return items;
}

// Called for each PLAYER_UPDATE we receive: bring our copies of that player's items in line with theirs
void AnchorItemSync_Apply(uint32_t clientId, const nlohmann::json& items) {
    if (gPlayState == nullptr || !Anchor::Instance->IsSaveLoaded()) {
        return;
    }

    if (gPlayState->sceneNum != sItemSyncScene) {
        sItemSyncScene = gPlayState->sceneNum;
        sFinishedCopies.clear();
    }

    if (GetDummyInScene(clientId) == nullptr) {
        KillCopiesOf(clientId);
        return;
    }

    std::set<uint32_t>& finished = sFinishedCopies[clientId];
    if (finished.size() > 256) {
        finished.clear(); // Ids only go up; old ones never come back
    }

    std::set<uint32_t> present;
    if (items.is_array()) {
        for (auto& entry : items) {
            if (!entry.is_array() || entry.size() < 10) {
                continue;
            }

            uint32_t itemId = entry[0].get<uint32_t>();
            s16 actorId = entry[1].get<s16>();
            if (!IsSyncedItem(actorId)) {
                continue;
            }

            present.insert(itemId);
            if (finished.contains(itemId)) {
                continue;
            }

            ItemSyncData state;
            state.pos = { entry[2].get<f32>(), entry[3].get<f32>(), entry[4].get<f32>() };
            state.rot = { entry[5].get<s16>(), entry[6].get<s16>(), entry[7].get<s16>() };
            state.extraA = entry[8].get<s16>();
            state.extraB = entry[9].get<s16>();

            Actor* copy = FindCopy(clientId, itemId);
            if (copy == nullptr) {
                // Arriving just as a bomb finishes blowing up: nothing left to show
                if (IsBomb(actorId) && state.extraB == BOMB_EXPLOSION && state.extraA < 8) {
                    finished.insert(itemId);
                    continue;
                }
                copy = SpawnCopy(clientId, itemId, actorId, state);
                if (copy == nullptr) {
                    continue;
                }
            }

            ItemSyncData* data = GetItemData(copy);
            data->pos = state.pos;
            data->rot = state.rot;
            data->extraA = state.extraA;
            data->extraB = state.extraB;
        }
    }

    // Their item is gone (caught the boomerang, put the hookshot away...): ours goes too. A bomb that's already
    // blowing up here finishes its explosion first.
    ForEachSyncedItem([&](Actor* actor) {
        ItemSyncData* data = GetItemData(actor);
        if (actor->update == NULL || data == nullptr || !data->copy || data->ownerClientId != clientId ||
            present.contains(data->ownerItemId)) {
            return;
        }
        if (IsBomb(actor->id) && actor->params == BOMB_EXPLOSION) {
            return;
        }
        Actor_Kill(actor);
    });
}

void Anchor::RegisterItemSyncHooks() {
    // An item created while our character (or one of our items, like a bombchu blowing up) is updating is ours
    COND_HOOK(OnActorSpawn, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (gPlayState == nullptr || !IsSyncedItem(actor->id) || sSpawningCopy || isProcessingIncomingPacket) {
            return;
        }

        // A bomb coming off a bomb flower in our game is ours too: we picked it up, hit it or lit it (another player
        // pulling one off their flower only empties ours, it never makes a bomb here)
        Player* self = GET_PLAYER(gPlayState);
        bool fromOurFlower = actor->id == ACTOR_EN_BOMBF && IsBombFlower(updatingActor);
        if (updatingActor == nullptr ||
            (updatingActor != &self->actor && !IsMine(updatingActor) && !fromOurFlower)) {
            return;
        }

        ItemSyncData data;
        data.mine = true;
        data.localId = sNextItemId++;
        ObjectExtension::GetInstance().Set<ItemSyncData>(actor, std::move(data));
    });

    // Copies don't run their own movement: they're placed where the owner's game says, every frame
    COND_HOOK(ShouldActorUpdate, isConnected, [&](void* refActor, bool* should) {
        Actor* actor = (Actor*)refActor;
        ItemSyncData* data = GetItemData(actor);
        if (data == nullptr || !data->copy) {
            return;
        }

        if (GetDummyInScene(data->ownerClientId) == nullptr) {
            Actor_Kill(actor);
            *should = false;
            return;
        }

        MakeHarmless(actor);

        switch (actor->id) {
            case ACTOR_EN_BOOM: {
                EnBoom* boomerang = (EnBoom*)actor;
                PlaceCopy(actor, data);
                actor->world.rot.x = data->extraA;
                actor->world.rot.y = data->extraB;
                boomerang->activeTimer++; // Spin
                Actor_SetFocus(actor, 0.0f);
                *should = false;
                break;
            }
            case ACTOR_EN_BOM_CHU:
                PlaceCopy(actor, data);
                ((EnBomChu*)actor)->timer = data->extraA; // Blinking
                *should = false;
                break;
            case ACTOR_ARMS_HOOK: {
                ArmsHook* hook = (ArmsHook*)actor;
                PlaceCopy(actor, data);
                hook->actionFunc = data->extraA != 0 ? ArmsHook_Shoot : ArmsHook_Wait; // Tip open or closed
                hook->timer = data->extraB;
                *should = false;
                break;
            }
            case ACTOR_EN_BOM:
            case ACTOR_EN_BOMBF:
                // Bombs run their own update (fizzing, flashing, the explosion), with the fuse lined up to the owner's
                if (actor->params == BOMB_BODY) {
                    PlaceCopy(actor, data);
                }
                SetCopyBombTimer(actor, data);
                break;
        }
    });

    // While a copied bomb updates, "the player" is its owner's character, so nothing it does involves us (offering to
    // be picked up, letting go of the player carrying it...)
    COND_HOOK(OnActorUpdateBegin, isConnected, [&](void* refActor, void** playerOverride) {
        Actor* actor = (Actor*)refActor;
        ItemSyncData* data = GetItemData(actor);
        if (data == nullptr || !data->copy) {
            return;
        }

        Player* owner = GetDummyInScene(data->ownerClientId);
        if (owner != nullptr) {
            *playerOverride = &owner->actor;
        }
    });

    COND_HOOK(OnActorUpdateEnd, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        ItemSyncData* data = GetItemData(actor);
        if (data == nullptr || !data->copy || !IsBomb(actor->id)) {
            return;
        }

        if (actor->params == BOMB_BODY) {
            PlaceCopy(actor, data); // Undo its own falling/rolling: it goes where the owner's is
        }
        MakeHarmless(actor);
    });

    // A copied bomb finished blowing up on its own: don't make a new one from the owner's fading explosion
    COND_HOOK(OnActorKill, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (!IsBomb(actor->id)) {
            return;
        }
        ItemSyncData* data = GetItemData(actor);
        if (data != nullptr && data->copy && actor->params == BOMB_EXPLOSION) {
            sFinishedCopies[data->ownerClientId].insert(data->ownerItemId);
        }
    });

    // Bomb flowers: notice when the bomb comes off one in our game, and tell everyone so theirs empties too
    COND_ID_HOOK(OnActorUpdateBegin, ACTOR_EN_BOMBF, isConnected, [&](void* refActor, void** playerOverride) {
        Actor* actor = (Actor*)refActor;
        if (!IsBombFlower(actor)) {
            return;
        }
        BombFlowerData data;
        data.hadBomb = ((EnBombf*)actor)->flowerBombScale >= 1.0f;
        ObjectExtension::GetInstance().Set<BombFlowerData>(actor, std::move(data));
    });

    COND_ID_HOOK(OnActorUpdateEnd, ACTOR_EN_BOMBF, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (!IsBombFlower(actor) || !IsSaveLoaded()) {
            return;
        }
        BombFlowerData* data = ObjectExtension::GetInstance().Get<BombFlowerData>(actor);
        if (data != nullptr && data->hadBomb && ((EnBombf*)actor)->flowerBombScale < 1.0f) {
            data->hadBomb = false;
            SendPacket_BombFlowerPlucked(actor);
        }
    });
}

void Anchor::SendPacket_BombFlowerPlucked(Actor* flower) {
    std::string key = AnchorEnemySync_GetKey(flower);
    if (key.empty()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = BOMB_FLOWER_PLUCKED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["key"] = key;
    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_BombFlowerPlucked(nlohmann::json payload) {
    if (!IsSaveLoaded() || payload.value("sceneNum", (s16)-1) != gPlayState->sceneNum) {
        return;
    }

    std::string key = payload.value("key", std::string());
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_PROP].head; actor != NULL; actor = actor->next) {
        if (actor->update == NULL || !IsBombFlower(actor) || AnchorEnemySync_GetKey(actor) != key) {
            continue;
        }

        EnBombf* flower = (EnBombf*)actor;
        if (flower->flowerBombScale < 1.0f || Actor_HasParent(actor, gPlayState)) {
            return; // Already empty here, or we're pulling it off ourselves right now
        }

        // Empty it and start it growing back, exactly like when the bomb is pulled off (no bomb is made here: the
        // one they pulled off comes through as their item)
        flower->timer = 180;
        flower->flowerBombScale = 0.0f;
        actor->flags &= ~ACTOR_FLAG_ATTENTION_ENABLED;
        Audio_PlayActorSound2(actor, NA_SE_PL_PULL_UP_ROCK);
        return;
    }
}
