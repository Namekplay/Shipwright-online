#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ObjectExtension.h"

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;

void func_8001E304(EnItem00* enItem00, PlayState* play); // The "pop out and bounce" a dropped item does
s16 func_8001F404(s16 dropId);                           // Swaps a drop for one you can use (no slingshot -> no seeds)
}

bool AnchorEnemySync_IsOtherPlayersKill(Actor* enemy);
bool AnchorObjectSync_IsOtherPlayersObject(Actor* object);

/**
 * Drop Sync (rupees, hearts, ammo dropped by grass, rocks, pots, crates and enemies)
 *
 * Items that pop out when you cut grass, lift a rock, break a pot or defeat an enemy used to only exist in your game.
 * Now they appear for everyone in the same scene, in the same spot and with the same bounce.
 *
 * The room owner picks how that works (Room Settings -> "Item Drops"):
 *  - Shared:       anyone can grab any drop. Whoever gets there first gets it, and it disappears for everyone else.
 *  - Visible only: you see other players' drops, but only the player whose drop it is can pick it up.
 *  - Off:          drops only exist for the player who caused them (how the game normally works).
 *
 * Only everyday pickups are shared (rupees, hearts, magic, bombs, arrows, seeds, nuts, sticks, bombchus). Anything
 * you need to progress - small keys, boss keys, heart pieces, heart containers, shields, tunics, randomizer items, or
 * anything the game tracks with a flag - is never copied, so nobody can take someone else's.
 *
 * When an enemy dies or a pot breaks in your game only because another player did it (their hit replayed here, or a
 * pot they threw), their game is the one that rolls and shares the drop, so yours doesn't make a second one.
 *
 * DROP_SPAWNED - Broadcast to the room when an item drops in your game
 * DROP_TAKEN   - Broadcast to the room when anyone picks up a shared drop
 */

namespace {

enum ItemDropMode : u8 {
    ITEM_DROPS_OFF = 0,
    ITEM_DROPS_VISIBLE = 1,
    ITEM_DROPS_SHARED = 2,
};

struct DropData {
    bool checked = false;           // We've looked at it on its first update
    bool fromOtherPlayer = false;   // It dropped because of something another player did (their game shares it)
    bool shared = false;            // It's a drop everyone can see
    bool mirror = false;            // It's a copy of another player's drop
    uint32_t ownerClientId = 0;     // Whose drop it is
    uint32_t id = 0;
};
static ObjectExtension::Register<DropData> DropDataRegister;

static uint32_t sNextDropId = 1;

u8 GetItemDropMode() {
    if (Anchor::Instance == nullptr || !Anchor::Instance->IsSaveLoaded()) {
        return ITEM_DROPS_OFF;
    }
    return Anchor::Instance->roomState.itemDropMode;
}

// Everyday pickups only. Nothing you need to progress.
bool IsShareableDropType(s16 type) {
    switch (type) {
        case ITEM00_RUPEE_GREEN:
        case ITEM00_RUPEE_BLUE:
        case ITEM00_RUPEE_RED:
        case ITEM00_RUPEE_ORANGE:
        case ITEM00_RUPEE_PURPLE:
        case ITEM00_HEART:
        case ITEM00_BOMBS_A:
        case ITEM00_BOMBS_B:
        case ITEM00_ARROWS_SINGLE:
        case ITEM00_ARROWS_SMALL:
        case ITEM00_ARROWS_MEDIUM:
        case ITEM00_ARROWS_LARGE:
        case ITEM00_NUTS:
        case ITEM00_STICK:
        case ITEM00_MAGIC_LARGE:
        case ITEM00_MAGIC_SMALL:
        case ITEM00_SEEDS:
        case ITEM00_BOMBCHU:
            return true;
        default:
            return false;
    }
}

bool IsShareableDrop(EnItem00* item) {
    return IsShareableDropType(item->actor.params) && item->collectibleFlag == 0 &&
           item->randoCheck == (RandomizerCheck)RC_UNKNOWN_CHECK && item->itemEntry.getItemId == GI_NONE &&
           item->actor.room == -1; // Dropped items are set to room -1 (placed items belong to a room)
}

EnItem00* FindSharedDrop(uint32_t ownerClientId, uint32_t id) {
    for (s32 category = 0; category < ACTORCAT_MAX; category++) {
        for (Actor* actor = gPlayState->actorCtx.actorLists[category].head; actor != NULL; actor = actor->next) {
            if (actor->id != ACTOR_EN_ITEM00 || actor->update == NULL) {
                continue;
            }
            DropData* data = ObjectExtension::GetInstance().Get<DropData>(actor);
            if (data != nullptr && data->shared && data->ownerClientId == ownerClientId && data->id == id) {
                return (EnItem00*)actor;
            }
        }
    }
    return nullptr;
}

} // namespace

void Anchor::RegisterDropSyncHooks() {
    // Note what caused each item to appear: an enemy dying or an object breaking only because of another player
    COND_ID_HOOK(OnActorSpawn, ACTOR_EN_ITEM00, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (gPlayState == nullptr || isProcessingIncomingPacket) {
            return; // Our own copies of other players' drops are tagged by whoever spawns them
        }

        DropData data;
        data.fromOtherPlayer = AnchorEnemySync_IsOtherPlayersKill(updatingActor) ||
                               AnchorObjectSync_IsOtherPlayersObject(updatingActor);
        ObjectExtension::GetInstance().Set<DropData>(actor, std::move(data));
    });

    // First update of a new item, before it has moved: share it, or remove a duplicate of someone else's drop
    COND_ID_HOOK(ShouldActorUpdate, ACTOR_EN_ITEM00, isConnected, [&](void* refActor, bool* should) {
        EnItem00* item = (EnItem00*)refActor;
        DropData* data = ObjectExtension::GetInstance().Get<DropData>(&item->actor);
        if (data == nullptr || data->checked || data->mirror) {
            return;
        }
        data->checked = true;

        u8 mode = GetItemDropMode();
        if (mode == ITEM_DROPS_OFF || !IsShareableDrop(item)) {
            return;
        }

        if (data->fromOtherPlayer) {
            // Their game dropped this same item and is sharing it with us; don't make a second one
            Actor_Kill(&item->actor);
            *should = false;
            return;
        }

        data->shared = true;
        data->ownerClientId = ownClientId;
        data->id = sNextDropId++;
        SendPacket_DropSpawned(item, data->id);
    });

    // Someone's about to pick up an item in our game
    COND_VB_SHOULD(VB_GIVE_ITEM_FROM_ITEM_00, isConnected, {
        EnItem00* item = va_arg(args, EnItem00*);
        DropData* data = ObjectExtension::GetInstance().Get<DropData>(&item->actor);
        if (data == nullptr || !data->shared || !*should) {
            return;
        }

        u8 mode = GetItemDropMode();
        if (data->mirror && mode != ITEM_DROPS_SHARED) {
            // "Visible only": this is another player's drop, so it isn't ours to take
            *should = false;
            return;
        }

        if (mode != ITEM_DROPS_OFF) {
            Anchor::Instance->SendPacket_DropTaken(data->ownerClientId, data->id);
        }
        data->shared = false; // Only once
    });
}

void Anchor::SendPacket_DropSpawned(EnItem00* item, uint32_t id) {
    nlohmann::json payload;
    payload["type"] = DROP_SPAWNED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["id"] = id;
    payload["item"] = item->actor.params;
    payload["pos"] = { item->actor.world.pos.x, item->actor.world.pos.y, item->actor.world.pos.z };
    payload["rotY"] = item->actor.world.rot.y;
    payload["velocityY"] = item->actor.velocity.y;
    payload["speedXZ"] = item->actor.speedXZ;
    payload["gravity"] = item->actor.gravity;
    payload["pop"] = item->actionFunc == func_8001E304;
    payload["timer"] = item->unk_15A;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_DropSpawned(nlohmann::json payload) {
    if (GetItemDropMode() == ITEM_DROPS_OFF || !payload.contains("clientId")) {
        return;
    }

    if (payload["sceneNum"].get<s16>() != gPlayState->sceneNum) {
        return;
    }

    s16 type = payload["item"].get<s16>();
    if (!IsShareableDropType(type)) {
        return;
    }

    // Same swap the game does for its own drops (e.g. bombs become bombchus when the randomizer says so). If it's
    // something we have no use for at all, it simply doesn't show up for us.
    type = func_8001F404(type);
    if (type < 0 || type == ITEM00_NONE) {
        return;
    }

    auto pos = payload["pos"];
    EnItem00* item = (EnItem00*)Actor_Spawn(&gPlayState->actorCtx, gPlayState, ACTOR_EN_ITEM00, pos[0].get<f32>(),
                                            pos[1].get<f32>(), pos[2].get<f32>(), 0, 0, 0, type);
    if (item == nullptr) {
        return;
    }

    DropData data;
    data.checked = true;
    data.shared = true;
    data.mirror = true;
    data.ownerClientId = payload["clientId"].get<uint32_t>();
    data.id = payload["id"].get<uint32_t>();
    ObjectExtension::GetInstance().Set<DropData>(&item->actor, std::move(data));

    // Pop out and bounce exactly like the original did
    item->actor.world.rot.y = payload["rotY"].get<s16>();
    item->actor.velocity.y = payload["velocityY"].get<f32>();
    item->actor.speedXZ = payload["speedXZ"].get<f32>();
    item->actor.gravity = payload["gravity"].get<f32>();
    item->actor.room = -1;
    item->actor.flags |= ACTOR_FLAG_UPDATE_CULLING_DISABLED;
    item->unk_15A = payload.value("timer", (s16)220);
    if (payload.value("pop", false)) {
        Actor_SetScale(&item->actor, 0.0f);
        item->actionFunc = func_8001E304;
    }
}

void Anchor::SendPacket_DropTaken(uint32_t ownerClientId, uint32_t id) {
    nlohmann::json payload;
    payload["type"] = DROP_TAKEN;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["owner"] = ownerClientId;
    payload["id"] = id;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_DropTaken(nlohmann::json payload) {
    if (Anchor::Instance == nullptr || !IsSaveLoaded() || payload["sceneNum"].get<s16>() != gPlayState->sceneNum) {
        return;
    }

    EnItem00* item = FindSharedDrop(payload["owner"].get<uint32_t>(), payload["id"].get<uint32_t>());
    if (item != nullptr) {
        Actor_Kill(&item->actor);
    }
}
