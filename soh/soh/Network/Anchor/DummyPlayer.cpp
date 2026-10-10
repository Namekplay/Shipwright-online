#include "Anchor.h"
#include "soh/Enhancements/nametag.h"
#include "soh/ObjectExtension/ObjectExtension.h"
#include "soh/ResourceManagerHelpers.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include "macros.h"
#include "variables.h"
#include "functions.h"
#include "objects/gameplay_keep/gameplay_keep.h"
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

// Crawling through a crawlspace. Your own character crawls in with the "enter crawlspace" animation, but once it's
// inside the game just holds the last frame of that animation and slides it along (you never see it, the camera is
// behind you). Other players can see it, so their characters loop the crawling part of that animation instead,
// moving the hands and knees in step with how far the character actually moved.
struct DummyPlayerCrawlState {
    bool valid = false;
    f32 frame = 0.0f;
    Vec3f lastPos = { 0.0f, 0.0f, 0.0f };
};
static ObjectExtension::Register<DummyPlayerCrawlState> DummyPlayerCrawlStateRegister;

constexpr f32 CRAWL_LOOP_START_FRAME = 40.0f; // The crawl steps in that animation run from frame 40...
constexpr f32 CRAWL_LOOP_END_FRAME = 104.0f;  // ...to 104, one hand/knee step every 8 frames
static f32 sCrawlFramesPerUnit = -1.0f;       // Animation frames per unit moved, worked out from the animation

static LinkAnimationHeader* GetCrawlAnimation() {
    void* anim = (void*)gPlayerAnim_link_child_tunnel_start;
    if (ResourceMgr_OTRSigCheck((char*)anim) != 0) {
        anim = ResourceMgr_LoadAnimByName((const char*)anim);
    }
    return (LinkAnimationHeader*)anim;
}

// Copy one frame of a Link animation (same layout the game uses when it loads a frame)
static void LoadLinkAnimFrame(LinkAnimationHeader* anim, s32 frame, Vec3s* out) {
    size_t frameSize = sizeof(Vec3s) * PLAYER_LIMB_MAX + 2;
    memcpy(out, (u8*)anim->segment + frameSize * frame, frameSize);
}

static void DummyPlayer_AnimateCrawl(Player* player, AnchorClient& client) {
    Actor* actor = &player->actor;
    DummyPlayerCrawlState* state = ObjectExtension::GetInstance().Get<DummyPlayerCrawlState>(actor);
    if (state == nullptr) {
        ObjectExtension::GetInstance().Set<DummyPlayerCrawlState>(actor, DummyPlayerCrawlState{});
        state = ObjectExtension::GetInstance().Get<DummyPlayerCrawlState>(actor);
    }

    // Only while inside the crawlspace (the entering/leaving animations already play normally)
    bool crawlingInside = (client.stateFlags2 & PLAYER_STATE2_CRAWLING) && client.movementFlags == 0 &&
                          client.linkAge == LINK_AGE_CHILD;
    if (!crawlingInside) {
        state->valid = false;
        return;
    }

    LinkAnimationHeader* anim = GetCrawlAnimation();
    if (anim == nullptr || anim->segment == nullptr) {
        return;
    }

    f32 lastFrame = Animation_GetLastFrame(anim);
    f32 loopEnd = std::min(CRAWL_LOOP_END_FRAME, lastFrame);
    if (loopEnd <= CRAWL_LOOP_START_FRAME) {
        return;
    }

    // How far the animation itself carries Link per frame while crawling, so the hands and knees keep pace
    if (sCrawlFramesPerUnit < 0.0f) {
        Vec3s startFrame[PLAYER_LIMB_BUF_COUNT];
        Vec3s endFrame[PLAYER_LIMB_BUF_COUNT];
        LoadLinkAnimFrame(anim, (s32)CRAWL_LOOP_START_FRAME, startFrame);
        LoadLinkAnimFrame(anim, (s32)loopEnd, endFrame);
        f32 rootDist = sqrtf(SQ((f32)(endFrame[0].x - startFrame[0].x)) + SQ((f32)(endFrame[0].z - startFrame[0].z)));
        f32 unitsPerFrame = rootDist * 0.01f / (loopEnd - CRAWL_LOOP_START_FRAME);
        sCrawlFramesPerUnit = (unitsPerFrame > 0.1f && unitsPerFrame < 10.0f) ? (1.0f / unitsPerFrame) : 0.7f;
    }

    if (!state->valid) {
        state->valid = true;
        state->frame = CRAWL_LOOP_START_FRAME;
    } else {
        // Forward/backward distance moved along the way the character faces
        f32 dx = actor->world.pos.x - state->lastPos.x;
        f32 dz = actor->world.pos.z - state->lastPos.z;
        f32 forward = dx * Math_SinS(actor->shape.rot.y) + dz * Math_CosS(actor->shape.rot.y);

        if (fabsf(forward) < 30.0f) { // Anything bigger is a teleport, not crawling
            f32 loopLength = loopEnd - CRAWL_LOOP_START_FRAME;
            state->frame += forward * sCrawlFramesPerUnit;
            while (state->frame >= loopEnd) {
                state->frame -= loopLength;
            }
            while (state->frame < CRAWL_LOOP_START_FRAME) {
                state->frame += loopLength;
            }
        }
    }
    state->lastPos = actor->world.pos;

    // Use the limb rotations from that frame; keep the root position the other player's game sent
    Vec3s frameJoints[PLAYER_LIMB_BUF_COUNT];
    LoadLinkAnimFrame(anim, (s32)state->frame, frameJoints);
    for (s32 i = 1; i < PLAYER_LIMB_MAX; i++) {
        client.jointTable[i] = frameJoints[i];
    }
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
    player->unk_6C2 = client.divePitch;
    player->unk_6C4 = client.sinkDepth;

    // No animation movement is applied here: the other player's game sends its position and pose after its own
    // animation movement (climbing, ledges, crawlspaces...) has already moved it, so the character is drawn exactly
    // where and how it is in their game. Moving it again here made climbing characters jitter up and down.

    DummyPlayer_AnimateCrawl(player, client);

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
