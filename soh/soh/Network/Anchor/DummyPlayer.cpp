#include "Anchor.h"
#include "soh/Enhancements/nametag.h"
#include "soh/ObjectExtension/ObjectExtension.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
extern PlayState* gPlayState;

void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
}

static DamageTable DummyPlayerDamageTable = {
    /* Deku nut      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Deku stick    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Slingshot     */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Explosive     */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Boomerang     */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Normal arrow  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Hammer swing  */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Hookshot      */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_STUN),
    /* Kokiri sword  */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master sword  */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant's Knife */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Fire arrow    */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice arrow     */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_ICE_TRAP),
    /* Light arrow   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Unk arrow 1   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 2   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Unk arrow 3   */ DMG_ENTRY(2, PLAYER_HIT_RESPONSE_NONE),
    /* Fire magic    */ DMG_ENTRY(0, DUMMY_PLAYER_HIT_RESPONSE_FIRE),
    /* Ice magic     */ DMG_ENTRY(3, PLAYER_HIT_RESPONSE_ICE_TRAP),
    /* Light magic   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_ELECTRIC_SHOCK),
    /* Shield        */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Mirror Ray    */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Kokiri spin   */ DMG_ENTRY(1, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant spin    */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master spin   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Kokiri jump   */ DMG_ENTRY(2, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Giant jump    */ DMG_ENTRY(8, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Master jump   */ DMG_ENTRY(4, DUMMY_PLAYER_HIT_RESPONSE_NORMAL),
    /* Unknown 1     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Unblockable   */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
    /* Hammer jump   */ DMG_ENTRY(4, PLAYER_HIT_RESPONSE_KNOCKBACK_LARGE),
    /* Unknown 2     */ DMG_ENTRY(0, PLAYER_HIT_RESPONSE_NONE),
};

void DummyPlayer_Init(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    // Hack to account for usage of gSaveContext in Player_Init
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = client.linkAge;

    // #region modeled after EnTorch2_Init and Player_Init
    actor->room = -1;
    player->itemAction = player->heldItemAction = -1;
    player->heldItemId = ITEM_NONE;
    Player_UseItem(play, player, ITEM_NONE);
    Player_SetModelGroup(player, Player_ActionToModelGroup(player, player->heldItemAction));
    play->playerInit(player, play, gPlayerSkelHeaders[client.linkAge]);

    play->func_11D54(player, play);
    // #endregion

    player->cylinder.base.acFlags = AC_ON | AC_TYPE_PLAYER;
    player->cylinder.base.ocFlags2 = OC2_TYPE_1;
    player->cylinder.info.bumperFlags = BUMP_ON | BUMP_HOOKABLE | BUMP_NO_HITMARK;
    player->actor.flags |= ACTOR_FLAG_HOOKSHOT_PULLS_PLAYER;
    player->cylinder.dim.radius = 30;
    player->actor.colChkInfo.damageTable = &DummyPlayerDamageTable;

    gSaveContext.linkAge = originalAge;

    bool isGlobalRoom = (std::string("soh-global") == CVarGetString(CVAR_REMOTE_ANCHOR("RoomId"), ""));

    if (!isGlobalRoom) {
        NameTag_RegisterForActorWithOptions(actor, client.name.c_str(), {});
    }
}

// Water ripples and splashes. Your own character makes these from its own movement in Player's update, which other
// players' characters don't run, so they're recreated here from how the character moves through the water.
struct DummyPlayerWaterState {
    bool valid = false;     // Has a previous position to compare against
    bool wasInWater = false; // Feet were under the water surface last update
    Vec3f lastPos = { 0.0f, 0.0f, 0.0f };
    f32 rippleDistance = 0.0f; // Distance moved through water since the last ripple
};
static ObjectExtension::Register<DummyPlayerWaterState> DummyPlayerWaterStateRegister;

static void DummyPlayer_UpdateWaterEffects(Player* player, PlayState* play) {
    Actor* actor = &player->actor;
    DummyPlayerWaterState* state = ObjectExtension::GetInstance().Get<DummyPlayerWaterState>(actor);
    if (state == nullptr) {
        ObjectExtension::GetInstance().Set<DummyPlayerWaterState>(actor, DummyPlayerWaterState{});
        state = ObjectExtension::GetInstance().Get<DummyPlayerWaterState>(actor);
    }

    f32 surfaceY = actor->world.pos.y;
    WaterBox* waterBox;
    bool inWater = WaterBox_GetSurface1(play, &play->colCtx, actor->world.pos.x, actor->world.pos.z, &surfaceY,
                                        &waterBox) &&
                   surfaceY > actor->world.pos.y;
    f32 depth = surfaceY - actor->world.pos.y;

    // A big jump in position means they teleported, respawned or just arrived: nothing to splash about
    if (state->valid && Math_Vec3f_DistXYZ(&state->lastPos, &actor->world.pos) > 200.0f) {
        state->valid = false;
    }

    if (inWater && state->valid) {
        f32 fallSpeed = state->lastPos.y - actor->world.pos.y;
        f32 moved = fabsf(actor->world.pos.x - state->lastPos.x) + fabsf(actor->world.pos.y - state->lastPos.y) +
                    fabsf(actor->world.pos.z - state->lastPos.z);
        f32 horizontalSpeed = sqrtf(SQ(actor->world.pos.x - state->lastPos.x) + SQ(actor->world.pos.z - state->lastPos.z));

        // Jumping or falling in: a splash where they hit the water
        if (!state->wasInWater && fallSpeed > 2.0f) {
            Vec3f splashPos = { actor->world.pos.x, surfaceY, actor->world.pos.z };
            EffectSsGSplash_Spawn(play, &splashPos, NULL, NULL, (fallSpeed <= 10.0f) ? 0 : 1, 400);
        }

        // Wading or swimming: a ripple every so often as they move, plus a splash when running through shallow water
        // (same spacing and sizes as your own character)
        if (depth < 50.0f || (player->stateFlags1 & PLAYER_STATE1_IN_WATER)) {
            state->rippleDistance += std::min(moved, 4.0f);

            if (state->rippleDistance > 15.0f) {
                state->rippleDistance = 0.0f;

                Vec3f ripplePos = { (Rand_ZeroOne() * 10.0f) + actor->world.pos.x, surfaceY,
                                    (Rand_ZeroOne() * 10.0f) + actor->world.pos.z };
                EffectSsGRipple_Spawn(play, &ripplePos, 100, 500, 0);

                if (horizontalSpeed > 4.0f && !(player->stateFlags1 & PLAYER_STATE1_IN_WATER) &&
                    surfaceY < player->bodyPartsPos[PLAYER_BODYPART_WAIST].y) {
                    Vec3f splashPos = { player->bodyPartsPos[PLAYER_BODYPART_WAIST].x, surfaceY,
                                        player->bodyPartsPos[PLAYER_BODYPART_WAIST].z };
                    EffectSsGSplash_Spawn(play, &splashPos, NULL, NULL, 0, (s16)((horizontalSpeed * 50.0f) + (depth * 5.0f)));
                }
            }
        }
    }

    state->valid = true;
    state->wasInWater = inWater;
    state->lastPos = actor->world.pos;
}

void Math_Vec3s_Copy(Vec3s* dest, Vec3s* src) {
    dest->x = src->x;
    dest->y = src->y;
    dest->z = src->z;
}

// Update the actor with new data from the client
void DummyPlayer_Update(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        actor->world.pos.x = -9999.0f;
        actor->world.pos.y = -9999.0f;
        actor->world.pos.z = -9999.0f;
        actor->shape.shadowAlpha = 0;
        return;
    }

    actor->shape.shadowAlpha = 255;
    Math_Vec3s_Copy(&player->upperLimbRot, &client.upperLimbRot);
    Math_Vec3s_Copy(&actor->shape.rot, &client.posRot.rot);
    Math_Vec3f_Copy(&actor->world.pos, &client.posRot.pos);
    player->skelAnime.jointTable = client.jointTable;
    player->skelAnime.movementFlags = client.movementFlags;
    Math_Vec3s_Copy(&player->skelAnime.prevTransl, &client.prevTransl);
    player->currentBoots = client.currentBoots;
    player->currentShield = client.currentShield;
    player->currentTunic = client.currentTunic;
    player->stateFlags1 = client.stateFlags1;
    player->stateFlags2 = client.stateFlags2;
    player->itemAction = client.itemAction;
    player->heldItemAction = client.heldItemAction;
    player->invincibilityTimer = client.invincibilityTimer;
    player->unk_862 = client.unk_862;
    player->unk_85C = client.unk_85C;
    player->av1.actionVar1 = client.actionVar1;

    // Apply animation movement (Copied from Player_ApplyAnimMovementScaledByAge)
    Vec3f diff;
    SkelAnime_UpdateTranslation(&player->skelAnime, &diff, player->actor.shape.rot.y);

    if (player->skelAnime.movementFlags & 1) {
        if (!LINK_IS_ADULT) {
            diff.x *= 0.64f;
            diff.z *= 0.64f;
        }

        player->actor.world.pos.x += diff.x * player->actor.scale.x;
        player->actor.world.pos.z += diff.z * player->actor.scale.z;
    }

    if (player->skelAnime.movementFlags & 2) {
        if (!(player->skelAnime.movementFlags & 4)) {
            diff.y *= player->ageProperties->unk_08;
        }

        player->actor.world.pos.y += diff.y * player->actor.scale.y;
    }

    DummyPlayer_UpdateWaterEffects(player, play);

    if (player->modelGroup != client.modelGroup) {
        // Hack to account for usage of gSaveContext
        s32 originalAge = gSaveContext.linkAge;
        gSaveContext.linkAge = client.linkAge;
        u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
        gSaveContext.equips.buttonItems[0] = client.buttonItem0;
        Player_SetModelGroup(player, client.modelGroup);
        gSaveContext.linkAge = originalAge;
        gSaveContext.equips.buttonItems[0] = originalButtonItem0;
    }

    if (Anchor::Instance->roomState.pvpMode == 0 ||
        (Anchor::Instance->roomState.pvpMode == 1 &&
         client.teamId == CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default"))) {
        actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
        return;
    }

    actor->flags &= ~ACTOR_FLAG_LOCK_ON_DISABLED;

    // Only our own attacks count as PvP hits. With enemy sync on, our copies of enemies can swing at other players'
    // characters too (and replayed hits are owned by them); their real game already handles those, so ignore them.
    Actor* attacker = player->cylinder.base.ac;
    bool hitByUs = attacker != NULL && attacker->category != ACTORCAT_ENEMY && attacker->category != ACTORCAT_BOSS &&
                   !(attacker->id == ACTOR_EN_OE2 && attacker->update == DummyPlayer_Update);

    if (player->cylinder.base.acFlags & AC_HIT && player->invincibilityTimer == 0 && hitByUs) {
        Anchor::Instance->SendPacket_DamagePlayer(client.clientId, player->actor.colChkInfo.damageEffect,
                                                  player->actor.colChkInfo.damage);
        if (player->actor.colChkInfo.damageEffect == DUMMY_PLAYER_HIT_RESPONSE_STUN) {
            Actor_SetColorFilter(&player->actor, 0, 0xFF, 0, 24);
        } else {
            player->invincibilityTimer = 20;
        }
    }

    Collider_UpdateCylinder(&player->actor, &player->cylinder);

    if (!(player->stateFlags2 & PLAYER_STATE2_FROZEN)) {
        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_HANGING_OFF_LEDGE |
                                     PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_ON_HORSE))) {
            CollisionCheck_SetOC(play, &play->colChkCtx, &player->cylinder.base);
        }

        if (!(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_DAMAGED)) &&
            (player->invincibilityTimer <= 0)) {
            CollisionCheck_SetAC(play, &play->colChkCtx, &player->cylinder.base);

            if (player->invincibilityTimer < 0) {
                CollisionCheck_SetAT(play, &play->colChkCtx, &player->cylinder.base);
            }
        }
    }

    if (player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_IN_ITEM_CS | PLAYER_STATE1_IN_CUTSCENE)) {
        player->actor.colChkInfo.mass = MASS_IMMOVABLE;
    } else {
        player->actor.colChkInfo.mass = 50;
    }

    Collider_ResetCylinderAC(play, &player->cylinder.base);
}

void DummyPlayer_Draw(Actor* actor, PlayState* play) {
    Player* player = (Player*)actor;

    uint32_t clientId = Anchor::Instance->GetDummyPlayerClientId(actor);

    if (!Anchor::Instance->clients.contains(clientId)) {
        Actor_Kill(actor);
        return;
    }

    AnchorClient& client = Anchor::Instance->clients[clientId];

    if (client.sceneNum != gPlayState->sceneNum || !client.online || !client.isSaveLoaded) {
        return;
    }

    // Hack to account for usage of gSaveContext in Player_Draw
    s32 originalAge = gSaveContext.linkAge;
    gSaveContext.linkAge = client.linkAge;
    u8 originalButtonItem0 = gSaveContext.equips.buttonItems[0];
    gSaveContext.equips.buttonItems[0] = client.buttonItem0;

    Player_Draw((Actor*)player, play);
    gSaveContext.linkAge = originalAge;
    gSaveContext.equips.buttonItems[0] = originalButtonItem0;
}

void DummyPlayer_Destroy(Actor* actor, PlayState* play) {
    // DummyPlayer Actors are initially spawned as ACTOR_PLAYER, but change their
    // ID shortly afterwards to ACTOR_EN_OE2. This would cause ACTOR_PLAYER's
    // ActorDB Entry's `numLoaded` to leak, which is mostly harmless but hits debug
    // asserts. Set the id back to ACTOR_PLAYER so that `numLoaded` will be decremented
    // correctly.
    actor->id = ACTOR_PLAYER;
}
