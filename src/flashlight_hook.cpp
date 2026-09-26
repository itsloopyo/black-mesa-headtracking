// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "flashlight_hook.h"

#include <cstdint>
#include <cstring>

#include "aim_state.h"
#include "builds/build_profile.h"
#include "debug_log.h"
#include "detour.h"
#include "game_state.h"
#include "log_throttle.h"
#include "plugin.h"
#include "source_math.h"

namespace headtracking {
namespace {

// The light renderer's flashlight update: __thiscall on the renderer, taking
// the eye origin, the player, and the forward/right/up eye vectors, all by
// pointer; it pops 20 bytes.
using UpdateFlashlightFn = void(__fastcall*)(void* renderer, void* edx, const float* origin,
                                            void* player, const float* forward,
                                            const float* right, const float* up);
using LocalPlayerFn = void*(__cdecl*)();

UpdateFlashlightFn g_original = nullptr;
LocalPlayerFn g_localPlayer = nullptr;
const AimState* g_view = nullptr;

struct PendingUpdate {
    void* renderer = nullptr;
    void* player = nullptr;
    float origin[3], forward[3], right[3], up[3];
};
PendingUpdate g_pending;

void UpdateTrackedLight(void* renderer, const float* origin, void* player,
                        const float* forward, const float* right, const float* up) {
    float trackedForward[3], trackedRight[3], trackedUp[3];
    if (g_view && g_view->applied) {
        source::AngleVectors(g_view->light_angles, trackedForward, trackedRight, trackedUp);
        origin = g_view->render_origin;
        forward = trackedForward;
        right = trackedRight;
        up = trackedUp;
        static LogThrottle throttle(2, 2, 600, 600);
        if (throttle.ShouldLog()) {
            HT_LOG("[flashlight] origin=(%.2f,%.2f,%.2f) forward=(%.3f,%.3f,%.3f)",
                   origin[0], origin[1], origin[2], forward[0], forward[1], forward[2]);
        }
    }
    g_original(renderer, nullptr, origin, player, forward, right, up);
}

void __fastcall HookUpdateFlashlight(void* renderer, void*, const float* origin, void* player,
                                     const float* forward, const float* right,
                                     const float* up) {
    // LightFollowsHead=false leaves the beam as the game aims it, from the game's own eye.
    if (player != g_localPlayer() || !GetPlugin().GetConfig().light.follows_head) {
        g_original(renderer, nullptr, origin, player, forward, right, up);
        return;
    }
    if (g_view) {
        UpdateTrackedLight(renderer, origin, player, forward, right, up);
        return;
    }
    if (!GetPlugin().IsEnabled() || !GetGameState().IsGameplayActive()) {
        g_pending.renderer = nullptr;
        g_original(renderer, nullptr, origin, player, forward, right, up);
        return;
    }
    // The player updates its light before RenderView samples the tracker. Defer
    // that update so the beam and camera consume the same frame's pose.
    g_pending.renderer = renderer;
    g_pending.player = player;
    std::memcpy(g_pending.origin, origin, sizeof(g_pending.origin));
    std::memcpy(g_pending.forward, forward, sizeof(g_pending.forward));
    std::memcpy(g_pending.right, right, sizeof(g_pending.right));
    std::memcpy(g_pending.up, up, sizeof(g_pending.up));
}

}  // namespace

bool InstallFlashlightHook(void* client, const builds::BuildProfile& profile) {
    const auto& offsets = profile.offsets;
    if (!offsets.flashlight_update_rva || !offsets.aim.local_player_rva) {
        HT_LOG("[flashlight] missing build offsets; beam follows the game's aim");
        return false;
    }
    auto* base = static_cast<uint8_t*>(client);
    g_localPlayer = reinterpret_cast<LocalPlayerFn>(base + offsets.aim.local_player_rva);
    return InstallDetour("flashlight", "UpdateFlashlight", base + offsets.flashlight_update_rva,
                         reinterpret_cast<void*>(&HookUpdateFlashlight),
                         reinterpret_cast<void**>(&g_original));
}

void BeginFlashlightView(const AimState& aim) {
    g_view = &aim;
    if (!g_pending.renderer) return;
    // A level change between the update and this frame may have replaced the
    // player object the saved pointer names.
    if (g_pending.player == g_localPlayer()) {
        UpdateTrackedLight(g_pending.renderer, g_pending.origin, g_pending.player,
                           g_pending.forward, g_pending.right, g_pending.up);
    }
    g_pending.renderer = nullptr;
}

void EndFlashlightView() { g_view = nullptr; }

}  // namespace headtracking
