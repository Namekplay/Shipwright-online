#include "soh/Network/Anchor/Anchor.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include "soh/OTRGlobals.h"

extern "C" {
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * TIME_SYNC
 *
 * Keeps the time of day the same for everyone in the room. Every couple of seconds each player sends their clock.
 * If another player's clock is ahead of ours (by up to half a day), we jump forward to match it. Clocks only ever move
 * forward, so a player standing in a dungeon (where time is frozen) gets pulled along by players out in the field,
 * and nobody's clock gets dragged backwards. Sun's Song from any player moves everyone's clock too.
 */

namespace {

// How far ahead another clock must be before we jump to it. dayTime runs 0x0000-0xFFFF over a full day; this is
// about 15 in-game minutes, comfortably more than what clocks drift apart during network lag.
constexpr s16 TIME_SYNC_THRESHOLD = 0x2AA;

// Send our clock every 40 player updates (~2 seconds at the game's 20 updates per second)
constexpr u32 TIME_SYNC_SEND_INTERVAL = 40;

u32 sTimeSyncFrameCounter = 0;

bool CanChangeTimeNow() {
    if (gPlayState == nullptr) {
        return false;
    }

    // Leave the clock alone during cutscenes, Sun's Song, scene transitions and when the real-time cheat is on
    return gPlayState->csCtx.state == CS_STATE_IDLE && gSaveContext.sunsSongState == SUNSSONG_INACTIVE &&
           gPlayState->transitionMode == TRANS_MODE_OFF &&
           // nextDayTime below 0xFF00 means a time change is already queued for the next scene load
           gSaveContext.nextDayTime >= 0xFF00 && !CVarGetInteger(CVAR_CHEAT("TimeSync"), 0);
}

} // namespace

void Anchor::TickTimeSync() {
    if (!roomState.syncTime || !IsSaveLoaded()) {
        return;
    }

    if (++sTimeSyncFrameCounter < TIME_SYNC_SEND_INTERVAL) {
        return;
    }
    sTimeSyncFrameCounter = 0;

    SendPacket_TimeSync();
}

void Anchor::SendPacket_TimeSync() {
    if (!roomState.syncTime || !IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = TIME_SYNC;
    payload["dayTime"] = gSaveContext.dayTime;
    payload["quiet"] = true;

    SendJsonToRemote(payload);
}

void Anchor::HandlePacket_TimeSync(nlohmann::json payload) {
    if (!roomState.syncTime || !IsSaveLoaded() || !CanChangeTimeNow()) {
        return;
    }

    u16 remoteTime = payload["dayTime"].get<u16>();

    // Signed 16-bit difference handles the wrap at midnight: positive means the other clock is ahead of ours
    s16 ahead = (s16)(remoteTime - gSaveContext.dayTime);
    if (ahead <= TIME_SYNC_THRESHOLD) {
        return;
    }

    gSaveContext.dayTime = remoteTime;
    gSaveContext.skyboxTime = remoteTime;
}
