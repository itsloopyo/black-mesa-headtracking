// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
// Render-view injection for Black Mesa (Source Engine, client.dll).
//
// Hook target: CViewRender::RenderView(CViewSetup* view, int clearFlags,
// int whatToDraw)  [__thiscall].  Discovered via MSVC RTTI: the CViewRender
// vftable, slot identified by the telemetry marker naming
// "CViewRender::RenderView". The arity is not a guess: the
// function ends in `ret 0xC`, so it pops exactly three stack arguments, and a
// detour declaring a fourth would leave the render thread's stack four bytes
// out on every frame.
//
// RenderView runs in the render phase, after the game has already produced the
// frame's CUserCmd / view angles (which drive aim, traces and weapon fire).
// We mutate the CViewSetup the renderer is about to consume - its angles,
// origin and FOV only - so the player sees the head-tracked view while the
// game's own camera (the player's eye angles) is untouched. Look and aim stay
// decoupled for free.
//
// Engagement is gated on a PE-fingerprint build-profile registry (append-only;
// see the "Maintain compatibility across new patches" doctrine and
// builds/build_registry.h). On any client.dll the registry does not recognise,
// the hook is never installed and the game runs vanilla.

#include "camera_hook.h"

#include <Windows.h>
#include <cmath>
#include <cstdint>

#include "aim_state.h"
#include "angles.h"
#include "builds/build_registry.h"
#include "cameraunlock/camera/lean_clamp.h"
#include "cameraunlock/effects/head_follow_light.h"
#include "cameraunlock/hooks/hook_manager.h"
#include "cameraunlock/memory/pe_fingerprint.h"
#include "cameraunlock/time/frame_clock.h"
#include "config.h"
#include "debug_log.h"
#include "detour.h"
#include "flashlight_hook.h"
#include "fov_override.h"
#include "game_state.h"
#include "lean_trace.h"
#include "log_throttle.h"
#include "plugin.h"
#include "source_math.h"
#include "view_setup.h"
#include "zoom_scale.h"

namespace headtracking {

namespace {

// ----- Resolved-at-load state ----------------------------------------------
const builds::BuildProfile* g_profile = nullptr;

using RenderViewFn = void(__fastcall*)(void* ecx, void* edx, void* view, int clearFlags,
                                       int whatToDraw);
RenderViewFn g_originalRenderView = nullptr;

// What the pose pipeline contributed to this frame's view, carried to the
// diagnostic line so it can report the delta alongside the resulting camera.
struct TrackingDelta {
    bool applied = false;
    float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;  // degrees, Source sign
    float x = 0.0f, y = 0.0f, z = 0.0f;           // Source units, camera basis
};

void Copy3(float* dst, const float* src) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
}

// ----- Tracker -> Source axis mapping ---------------------------------------
//
// Every sign correction between the tracker frame and Source lives here, at
// the engine boundary, after the processor's asymmetric Z clamp. Flipping z
// before it would move the generous PositionLimitZ (0.40m) allowance onto the
// backward lean and leave PositionLimitZBack (0.10m) for leaning in. The
// direction still looks right, which is why that shape survives testing - the
// only symptom is that leaning in barely moves.
//
// Tracker frame, as the pipeline delivers it:
//     yaw   > 0  = head turns right   Source yaw   > 0 = turn left   -> negate
//     pitch > 0  = head looks up      Source pitch > 0 = look down   -> negate
//     roll  > 0  = head tilts left    Source roll  > 0 = tilt right  -> negate
//     x     > 0  = head moves left    Source right vector            -> negate
//     y     > 0  = head moves up      Source up vector               -> as-is
//     z     < 0  = head leans forward Source forward vector          -> negate
constexpr float kYawSign   = -1.0f;
constexpr float kPitchSign = -1.0f;
constexpr float kRollSign  = -1.0f;
constexpr float kPosXSign  = -1.0f;
constexpr float kPosYSign  =  1.0f;
constexpr float kPosZSign  = -1.0f;

// ----- Diagnostics ----------------------------------------------------------

// Dense at first (the first frames, then ~every 200), because that is where
// install-time faults show; then one line per ~2000 frames for the rest of the
// session. See log_throttle.h for why the schedule has that shape.
constexpr int kBurstLines           = 6;     // opening frames logged unconditionally
constexpr int kEarlyLines           = 30;    // lines still treated as install-time
constexpr int kEarlyIntervalFrames  = 200;
constexpr int kSteadyIntervalFrames = 2000;

// Confirms the hook fires, the offsets resolve to a sane camera, and the
// head-tracking delta is being applied. Reads the CViewSetup after the delta
// has been written, so the line reports the view the frame will render with.
//
// Both FOVs are on it because that is the mod's read of the camera's field of
// view: a wrong CViewSetup::fov offset shows up here as a number that is not
// the player's fov_desired widened for their window, and there is nothing else
// in the log that would catch it.
void DiagnosticLog(const ViewSetup& view, const TrackingDelta& delta) {
    static LogThrottle s_throttle(kBurstLines, kEarlyLines, kEarlyIntervalFrames,
                                  kSteadyIntervalFrames);
    if (!s_throttle.ShouldLog()) return;

    const float* org = view.Origin();
    const float* ang = view.Angles();
    HT_LOG("[view] rect=%dx%d org=(%.1f,%.1f,%.1f) ang=(p%.2f y%.2f r%.2f) fov=%.2f/%.2f "
           "| track=%d delta=(p%.2f y%.2f r%.2f) pos=(%.2f,%.2f,%.2f)",
           view.RectWidth(), view.RectHeight(), org[0], org[1], org[2], ang[0], ang[1], ang[2],
           view.Fov(), view.FovViewmodel(), delta.applied ? 1 : 0,
           delta.pitch, delta.yaw, delta.roll, delta.x, delta.y, delta.z);
}

// ----- Lean collision -------------------------------------------------------
//
// A lean must never put the eye inside the level. The clean eye is where the
// game put the camera, which the player hull already keeps clear of walls; the
// lean is swept from there and cut to whatever the level leaves room for,
// before it is applied.

// Set once, before the detour is armed: CollisionEnabled is on and the trace
// resolved on this build.
bool g_leanTraceReady = false;
cameraunlock::camera::LeanClamp g_leanClamp;
lean_trace::Context g_leanContext;
cameraunlock::time::FrameClock g_leanClock;

// A clean eye that moves further than this in one frame has been teleported or
// the level has changed under it, and the allowance carried from the last
// frame describes a wall that is no longer there.
constexpr float kCameraCutUnits = 128.0f;
float g_lastCleanEye[3] = {0.0f, 0.0f, 0.0f};
bool g_haveLastCleanEye = false;

// Contact comes and goes as often as the head brushes a wall, so a change is
// written at most once per this many frames, and always eventually.
constexpr int kContactLogIntervalFrames = 60;
int g_leanFrame = 0;
int g_lastContactLogFrame = -kContactLogIntervalFrames;
bool g_loggedContact = false;
bool g_loggedQueryFailed = false;

// The steady sample beside the transitions: they alone cannot tell "the sweep
// runs and the room is open" from "the sweep is not running".
constexpr int kLeanBurstLines           = 2;
constexpr int kLeanEarlyLines           = 10;
constexpr int kLeanEarlyIntervalFrames  = 600;
constexpr int kLeanSteadyIntervalFrames = 3000;

void ResetLeanClamp() {
    g_leanClamp.Reset();
    g_haveLastCleanEye = false;
}

// The standoff has to clear the near plane's CORNER, not only the plane: a
// wall seen at a glancing angle crosses the corner first. Read off the frame's
// own projection, logged once, and warned about once if the margin falls
// inside it.
void CheckStandoff(const ViewSetup& view) {
    static bool s_logged = false;
    static bool s_warned = false;
    if (s_warned) return;

    const float zNear = view.ZNear();
    const float fov = view.Fov();
    const int w = view.RectWidth();
    const int h = view.RectHeight();
    if (!(zNear > 0.0f) || !(fov > 0.0f && fov < 179.0f) || w <= 0 || h <= 0) {
        if (!s_logged) {
            s_logged = true;
            HT_LOG("[lean] the near plane reads as %.2f at %.2f degrees in %dx%d - the margin cannot "
                   "be checked against it", zNear, fov, w, h);
        }
        return;
    }
    const float tanH = std::tan(fov * 0.5f * kDegToRad);
    const float tanV = tanH * static_cast<float>(h) / static_cast<float>(w);
    const float corner = zNear * std::sqrt(1.0f + tanH * tanH + tanV * tanV);
    if (!s_logged) {
        s_logged = true;
        HT_LOG("[lean] near plane %.2f units out, its corner %.2f at %.2f degrees in %dx%d; "
               "CollisionMargin %.2f", zNear, corner, fov, w, h, g_leanContext.skin);
    }
    if (g_leanContext.skin < corner) {
        s_warned = true;
        HT_LOG("[lean] WARN: CollisionMargin %.2f is inside the near plane's corner (%.2f units at "
               "%.2f degrees in %dx%d) - a wall seen at a glancing angle can still go transparent "
               "while you lean", g_leanContext.skin, corner, fov, w, h);
    }
}

void LogLeanClamp(float asked, float kept) {
    ++g_leanFrame;
    const bool failed = g_leanClamp.LastQueryFailed();
    if (failed != g_loggedQueryFailed) {
        g_loggedQueryFailed = failed;
        HT_LOG(failed ? "[lean] the wall check could not run - the lean passes through unclamped"
                      : "[lean] the wall check is running again");
    }
    const bool contact = g_leanClamp.InContact();
    if (contact != g_loggedContact &&
        g_leanFrame - g_lastContactLogFrame >= kContactLogIntervalFrames) {
        g_loggedContact = contact;
        g_lastContactLogFrame = g_leanFrame;
        if (contact) {
            const lean_trace::LastHit hit = lean_trace::Last();
            HT_LOG("[lean] held off a surface: asked %.1f units, kept %.1f (hit %.1f along the "
                   "lean, approach cos %.2f, margin %.1f)",
                   asked, kept, hit.distance, hit.cosine, g_leanContext.skin);
        } else {
            HT_LOG("[lean] clear: the full lean is back");
        }
    }
    static LogThrottle s_throttle(kLeanBurstLines, kLeanEarlyLines, kLeanEarlyIntervalFrames,
                                  kLeanSteadyIntervalFrames);
    if (s_throttle.ShouldLog()) {
        HT_LOG("[lean] sample asked=%.2f kept=%.2f contact=%d failed=%d", asked, kept,
               contact ? 1 : 0, failed ? 1 : 0);
    }
}

// Cuts `offset` (world units, added to `eye`) to what the level leaves room
// for, and returns the fraction of it kept.
float ClampLeanToWorld(const ViewSetup& view, const float* eye, float* offset) {
    const float dt = g_leanClock.Tick();
    if (!g_leanTraceReady) return 1.0f;

    if (g_haveLastCleanEye) {
        const float dx = eye[0] - g_lastCleanEye[0];
        const float dy = eye[1] - g_lastCleanEye[1];
        const float dz = eye[2] - g_lastCleanEye[2];
        if (dx * dx + dy * dy + dz * dz > kCameraCutUnits * kCameraCutUnits) g_leanClamp.Reset();
    }
    Copy3(g_lastCleanEye, eye);
    g_haveLastCleanEye = true;

    CheckStandoff(view);

    using cameraunlock::math::Vec3;
    const Vec3 desired(offset[0], offset[1], offset[2]);
    const Vec3 kept = g_leanClamp.Apply(Vec3(eye[0], eye[1], eye[2]), desired, dt,
                                        &lean_trace::Query, &g_leanContext);
    const float asked = desired.Magnitude();
    const float allowed = kept.Magnitude();
    LogLeanClamp(asked, allowed);

    offset[0] = kept.x;
    offset[1] = kept.y;
    offset[2] = kept.z;
    return asked > 0.0f ? allowed / asked : 1.0f;
}

// ----- Pose injection -------------------------------------------------------

// Shifts the render origin in the CLEAN view basis - the one built from the
// angles before the head delta - so the lean follows the body rather than the
// head-rotated view, then cuts it to what the level leaves room for. `delta`
// receives the applied offset in Source units for the diagnostic line.
void ApplyPositionalLean(const Plugin& plugin, const ViewSetup& view, const float* cleanAngles,
                         float* org, float zoomFactor, TrackingDelta& delta) {
    if (!plugin.GetPositionOffset(delta.x, delta.y, delta.z)) {
        ResetLeanClamp();
        return;
    }

    float fwd[3], right[3], up[3];
    source::AngleVectors(cleanAngles, fwd, right, up);

    // A head offset seen at depth D lands at d / (2 * D * tan(fov/2)) of the
    // frame, so its displacement on screen is linear in both the offset and the
    // zoom factor and the factor multiplies it directly. Applied after the lean
    // envelope, which clamps how far the HEAD may move; this is how much of that
    // the rendered eye spends to put the picture in the same place.
    delta.x *= kPosXSign * zoomFactor;
    delta.y *= kPosYSign * zoomFactor;
    delta.z *= kPosZSign * zoomFactor;
    float offset[3];
    for (int i = 0; i < 3; ++i) {
        offset[i] = right[i] * delta.x + up[i] * delta.y + fwd[i] * delta.z;
    }
    const float kept = ClampLeanToWorld(view, org, offset);
    delta.x *= kept;
    delta.y *= kept;
    delta.z *= kept;
    for (int i = 0; i < 3; ++i) org[i] += offset[i];
}

// Composes the head rotation onto the render view's QAngle, in the yaw mode the
// player has selected. `delta` receives the applied rotation in Source degrees.
void ApplyRotationDelta(const Plugin& plugin, float yawRad, float pitchRad, float rollRad,
                        float zoomFactor, float* ang, float* lightAngles, TrackingDelta& delta) {
    // Yaw and pitch translate the picture across the frame, so both are scaled
    // for whatever FOV this frame is being rendered at. Roll rotates it about
    // the view axis by the same angle at every FOV there is, so roll is not.
    delta.pitch = ScaleRotationForZoom(pitchRad * kRadToDeg, zoomFactor) * kPitchSign;
    delta.yaw   = ScaleRotationForZoom(yawRad   * kRadToDeg, zoomFactor) * kYawSign;
    delta.roll  = rollRad  * kRadToDeg * kRollSign;
    const auto light = cameraunlock::effects::ScaleHeadEuler(
        {delta.yaw, delta.pitch, delta.roll}, plugin.GetConfig().light.multiplier);

    if (plugin.IsWorldSpaceYaw()) {
        // Source QAngle is intrinsically horizon-locked - yaw is about world
        // up, pitch about the yawed right axis - so adding the head delta
        // straight on IS the world-space-yaw composition.
        ang[0] += delta.pitch;
        ang[1] += delta.yaw;
        ang[2] += delta.roll;
        lightAngles[0] += light.pitch;
        lightAngles[1] += light.yaw;
        lightAngles[2] += light.roll;
    } else {
        source::ApplyCameraLocalRotation(ang, delta.pitch, delta.yaw, delta.roll);
        source::ApplyCameraLocalRotation(lightAngles, light.pitch, light.yaw, light.roll);
    }
}

// ----- The hook -------------------------------------------------------------

// The tracking work, separated from the detour so the original call can sit
// outside the try. Nothing here is expected to throw, but the pipeline touches
// std::string and std::function, and an exception unwinding out of a __fastcall
// detour through a MinHook trampoline into client.dll frames would skip the
// original RenderView - a black screen, then terminate. Swallowing is the one
// correct answer here: a dropped frame of head tracking is nothing, a frame the
// engine never renders is everything.
void ApplyTracking(const ViewSetup& view) {
    Plugin& plugin = GetPlugin();
    // Unconditional, including in menus: it is what advances the frame clock
    // and drains the socket, so suspending it would hand the pipeline one huge
    // dt and a backlog of stale samples on the way back into gameplay.
    plugin.Update();

    const bool active = plugin.IsEnabled() && GetGameState().IsGameplayActive();

    float* org = view.Origin();
    float* ang = view.Angles();

    AimState aim;
    Copy3(aim.clean_origin, org);
    Copy3(aim.clean_angles, ang);
    Copy3(aim.light_angles, ang);

    TrackingDelta delta;
    if (active) {
        // The FOV is not gated on tracker data - it is a view setting, not a
        // pose, and a player whose tracker is asleep still wants the frame they
        // configured. It IS gated on the tracking toggle, so End leaves a
        // completely vanilla view behind rather than a vanilla view at a
        // modded FOV.
        const Config& config = plugin.GetConfig();
        // First, because it settles what this frame's FOV is - both the
        // override's write and the factor the pose is scaled by come out of it,
        // and the pose has to be scaled for the FOV the frame ACTUALLY renders
        // at, override included.
        const float zoom = PrepareFrameFov(view, config.fov_override);

        float yawRad, pitchRad, rollRad;
        if (plugin.GetRotationRadians(yawRad, pitchRad, rollRad)) {
            delta.applied = true;
            // Position first: it reads the clean angles, which the rotation
            // below overwrites in place.
            ApplyPositionalLean(plugin, view, aim.clean_angles, org, zoom, delta);
            ApplyRotationDelta(plugin, yawRad, pitchRad, rollRad, zoom, ang, aim.light_angles,
                               delta);
        } else {
            ResetLeanClamp();
        }
    } else {
        ResetLeanClamp();
    }

    // Published unconditionally, including the untracked case: a stale state
    // left behind after tracking stops would hold the crosshair off-centre with
    // nothing moving the view any more.
    aim.applied = delta.applied;
    Copy3(aim.render_origin, org);
    Copy3(aim.render_angles, ang);
    PublishAimState(aim);

    DiagnosticLog(view, delta);
}

void __fastcall Hook_RenderView(void* ecx, void* edx, void* view, int clearFlags,
                                int whatToDraw) {
    if (view) {
        try {
            ApplyTracking(ViewSetup(view, g_profile->offsets.view_setup));
        } catch (...) {
            // Deliberately silent: logging from here could throw again, and the
            // only thing that matters is reaching the original call below.
        }
    }

    if (view) BeginFlashlightView(CurrentAimState());
    g_originalRenderView(ecx, edx, view, clearFlags, whatToDraw);
    EndFlashlightView();
}

// ----- Installation ---------------------------------------------------------

// client.dll is loaded long after the ASI, so the bootstrap thread waits for it
// rather than giving up on the first miss.
constexpr int   kClientWaitAttempts   = 200;
constexpr DWORD kClientWaitIntervalMs = 100;

HMODULE WaitForClientModule() {
    for (int i = 0; i < kClientWaitAttempts; ++i) {
        if (HMODULE client = GetModuleHandleA("client.dll")) return client;
        Sleep(kClientWaitIntervalMs);
    }
    return nullptr;
}

// Fingerprints the running client.dll and returns its profile, or nullptr -
// which is the dormant path: the game runs vanilla and the log says why.
const builds::BuildProfile* ResolveBuildProfile(HMODULE client) {
    cameraunlock::memory::PeFingerprint fp{};
    if (!cameraunlock::memory::ReadPeFingerprint(client, fp)) {
        HT_LOG("[hook] could not read client.dll fingerprint");
        return nullptr;
    }
    HT_LOG("[hook] client.dll fingerprint TimeDateStamp=0x%08X SizeOfImage=0x%08X CheckSum=0x%08X",
           fp.TimeDateStamp, fp.SizeOfImage, fp.CheckSum);

    const builds::BuildProfile* profile = builds::MatchProfile(fp);
    if (!profile) {
        builds::LogUnrecognisedBuild(fp);
        return nullptr;
    }
    if (!profile->IsComplete()) {
        HT_LOG("[hook] build profile '%s' is a placeholder (hook target not yet rederived) "
               "- staying dormant", profile->name);
        return nullptr;
    }
    HT_LOG("[hook] matched build profile '%s'", profile->name);
    return profile;
}

void ResolveLeanCollision() {
    const Config& config = GetPlugin().GetConfig();
    if (!config.collision_enabled) {
        HT_LOG("[lean] CollisionEnabled=false - leaning is not stopped by walls");
        return;
    }
    if (!lean_trace::Resolve(*g_profile)) return;
    g_leanClamp.SetSettings(config.lean_clamp);
    g_leanContext.skin = config.lean_clamp.skin;
    g_leanTraceReady = true;
    HT_LOG("[lean] wall check on: margin %.2f units, release smoothing %.2f",
           config.lean_clamp.skin, config.lean_clamp.release_smoothing);
}

// This is the mod's first hook, so it is where MinHook itself is brought up.
bool InstallRenderViewDetour(void* target) {
    using cameraunlock::hooks::HookManager;
    using cameraunlock::hooks::HookStatus;

    if (HookManager::Instance().Initialize() != HookStatus::Ok) {
        HT_LOG("[hook] MinHook init failed");
        return false;
    }
    return InstallDetour("hook", "RenderView", target,
                         reinterpret_cast<void*>(&Hook_RenderView),
                         reinterpret_cast<void**>(&g_originalRenderView));
}

}  // namespace

bool CameraHook::Install() {
    HMODULE client = WaitForClientModule();
    if (!client) {
        HT_LOG("[hook] client.dll never loaded");
        return false;
    }

    // Published before the detour is armed: the very first RenderView can land
    // inside EnableHook, and it dereferences this.
    g_profile = ResolveBuildProfile(client);
    if (!g_profile) return false;

    // Before the detour too, and fatal if it fails: the gate is what keeps the
    // pose out of the menu backdrop and out of a multiplayer session, so a hook
    // installed without one is worse than no hook at all.
    if (!GetGameState().Resolve()) return false;

    ResolveFovConVars(client, *g_profile);
    ResolveLeanCollision();

    void* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(client)
                                           + g_profile->offsets.render_view_rva);
    if (!InstallRenderViewDetour(target)) return false;
    InstallFlashlightHook(client, *g_profile);
    return true;
}

}  // namespace headtracking
