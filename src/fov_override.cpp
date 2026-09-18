// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
//
// CViewSetup::fov is the horizontal FOV, in degrees, the frame is actually
// rendered with, and it is the mod's answer to "what is the camera's FOV". The
// view has already taken the player's fov_desired - which Source defines
// against a 4:3 screen - and widened it for the real viewport by the time
// RenderView sees the struct, and it has already applied whatever the game
// itself is doing to the FOV that frame (the suit zoom, a scripted sequence).
// So this one float is the live value, not a setting.
//
// It is also the right place to CHANGE it. Everything the frame is built from
// comes out of this struct, including the world-to-screen matrix, so an
// override written here needs nothing kept in sync with it.
//
// The game's own knob is the fov_desired cvar, which reads 90 on this build and
// which the game declares as a 20-120 slider in bms\cfg\user_default.scr.
//
// The override is a RATIO against that cvar, not a value written over the top
// of whatever the frame happens to hold. CViewSetup::fov is not always the
// player's FOV: the suit zoom (+zoom, bound to Z in the game's own
// config.cfg), a weapon zoom and a scripted camera each write their own.
// Writing the configured number in unconditionally would flatten every one of
// them, so the zoom key would visibly do nothing. Scaling by (configured /
// cvar) instead shows exactly the configured number on a frame at the player's
// own FOV, scales a zoom by the same factor as everything else, and is
// continuous through the transition, so there is no frame where the override
// snaps in or out.
//
// The scaling happens in the cvar's own 4:3 reference rather than on the
// rendered value: the widening is a tangent scaling, so a ratio applied to the
// widened number is not the ratio the player asked for.
//
// The same struct answers the second FOV question the mod has, which is what
// the HEAD POSE has to be scaled by. A narrow field of view magnifies the whole
// frame, head tracking with it, so the same head angle sweeps further across
// the screen the moment the game zooms and the player reads that as the mod's
// sensitivity jumping. The correction is the ratio between the FOV being
// rendered and the one an un-zoomed frame would be - see ZoomFactor below, and
// zoom_scale.h for what is done with it.
//
// That factor is measured against the OVERRIDDEN un-zoomed FOV rather than the
// player's raw fov_desired. A player who sets Fov=110 has chosen a wider
// ordinary view, not a permanent 0.75x on their head tracking.

#include "fov_override.h"

#include <cmath>
#include <cstdint>
#include <cstring>

#include "angles.h"
#include "cameraunlock/camera/zoom_compensation.h"
#include "debug_log.h"
#include "source_math.h"

namespace headtracking {

namespace {

// At and past this the projection degenerates - tan(fov/2) runs away - so a
// frame that scales into it is left as the game rendered it.
constexpr float kMaxRenderableFov = 179.0f;

// The resolved fov_desired ConVar object, and where its value sits inside it.
// The object rather than a pointer straight at the float, because the value has
// to be decoded on every read - see ReadObfuscatedFloat.
const uint8_t* g_fovDesiredObject = nullptr;
uint32_t g_convarValueOffset = 0;

// Black Mesa does not store ConVar values in the clear. The dword at
// ConVar::m_fValue is the float's bits XORed with the ConVar object's own
// address, and the engine's own inlined readers undo it exactly this way. A
// mod that read the dword as a float would get 1.4e28 where the player has 90,
// and - because the number is still finite and positive - would scale the frame
// by it rather than noticing.
float ReadObfuscatedFloat(const uint8_t* object, uint32_t valueOffset) {
    const uint32_t bits = *reinterpret_cast<const uint32_t*>(object + valueOffset)
                          ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(object));
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// A ConVar object whose name reads back as the expected string proves the rest
// of the layout fits. A plausible-looking float proves nothing, which is
// exactly the failure this check exists to catch - and here it could not, since
// the raw dword is not a plausible float at all.
//
// The parent check is the engine's own precondition, not an extra: the game
// decodes the inline value only when m_pParent is the object itself, and calls
// the parent's virtual GetFloat otherwise. A child ConVar's own dword is not
// the live value, so decoding it would be reading the wrong number.
const uint8_t* ResolveConVar(HMODULE client, const builds::FovConVarOffsets& off, uint32_t rva,
                             const char* expectedName) {
    const uint8_t* object = reinterpret_cast<const uint8_t*>(client) + rva;
    const char* name = *reinterpret_cast<const char* const*>(object + off.convar_name);
    if (!name || std::strcmp(name, expectedName) != 0) {
        HT_LOG("[view] client.dll+0x%X does not read as the %s ConVar (its name is '%s') - the "
               "[View] Fov key is inert this session and the head pose is not scaled for "
               "zoom; head tracking is otherwise unaffected",
               rva, expectedName, name ? name : "(null)");
        return nullptr;
    }
    const void* parent = *reinterpret_cast<const void* const*>(object + off.convar_parent);
    if (parent != object) {
        HT_LOG("[view] the %s ConVar at client.dll+0x%X is a child of %p, so its own stored "
               "value is not the live one - the [View] Fov key is inert this session and the "
               "head pose is not scaled for zoom",
               expectedName, rva, parent);
        return nullptr;
    }
    return object;
}

// Per-field diagnostic state. `logged_factor` fires once per factor - when the
// player edits the cvar, not on every frame of a zoom - and `refusal_logged`
// once per field.
struct FieldLog {
    float logged_factor = 0.0f;
    bool refusal_logged = false;
};

// The rendered viewport, with the width ratio the engine widens fov_desired by
// derived from it rather than passed alongside it - the two cannot disagree.
struct Viewport {
    Viewport(int width, int height)
        : w(width),
          h(height),
          ratio((static_cast<float>(width) / static_cast<float>(height))
                * source::kReferenceAspectInverse) {}

    int w;
    int h;
    float ratio;
};

// Scales one of the view's FOV fields. True when the field was actually
// rewritten, which is what tells the caller whether an un-zoomed frame now
// renders at the configured FOV or still at the game's own.
bool ScaleField(float& target, float desired, float base, const Viewport& viewport,
                const char* what, FieldLog& log) {
    if (desired <= 0.0f) return false;
    // The game renders a zero FOV on the frames where the field is not in use.
    // There is nothing to scale, and a ratio against a zero cvar has no meaning
    // either.
    if (!(base > 0.0f) || !std::isfinite(target) || target <= 0.0f) return false;

    const float factor = desired / base;
    const float rendered = source::ScaleFovByWidthRatio(
        source::UnscaleFovByWidthRatio(target, viewport.ratio) * factor, viewport.ratio);
    if (!std::isfinite(rendered) || rendered <= 0.0f || rendered >= kMaxRenderableFov) {
        // This frame only. The game is rendering something wide enough that the
        // player's factor takes it past having a projection at all - a
        // cinematic, not a broken offset - so the honest answer is the frame the
        // game built, not an override disabled for the rest of the run.
        if (!log.refusal_logged) {
            log.refusal_logged = true;
            HT_LOG("[view] %s FOV left alone on a frame the game rendered at %.2f: scaling "
                   "it by %.3f gives %.2f degrees, which has no projection",
                   what, target, factor, rendered);
        }
        return false;
    }
    if (log.logged_factor != factor) {
        log.logged_factor = factor;
        HT_LOG("[view] %s FOV override: %.1f over the game's %.1f = x%.3f, so a normal "
               "frame renders at %.2f in a %dx%d viewport", what, desired, base, factor,
               source::ScaleFovByWidthRatio(desired, viewport.ratio), viewport.w, viewport.h);
    }
    target = rendered;
    return true;
}

// ----- Zoom compensation ----------------------------------------------------

// A frame at the un-zoomed FOV. The pose factor is 1.0 there, which is the
// whole of ordinary play.
constexpr float kNoZoomScaling = 1.0f;

// How far the factor has to move before it is worth another line. Wide enough
// that the float noise between the engine's widening and ours is not a zoom -
// the two compute the same tangent scaling from the same fov_desired with the
// same operations in a different order, so they agree to about a part in a
// million rather than exactly - and narrow enough that a zoom is reported while
// it is still sweeping.
constexpr float kZoomLogBand = 0.02f;

// Zero, not 1.0, so the opening frame always logs: that line is the gate, and a
// factor that starts at exactly 1.0 is the case it most needs to prove.
struct ZoomLog {
    float logged_factor = 0.0f;
};

float TanHalf(float fovDegrees) { return std::tan(fovDegrees * 0.5f * kDegToRad); }

// Every term of the factor on one line, because a factor that is wrong by a
// constant reads exactly like a factor that is right. The units are on it for
// the same reason: the way this goes wrong is a live FOV and a base measured on
// different axes, which leaves normal play running at a fixed fraction of the
// pose with no symptom but head tracking feeling weak everywhere.
//
// Written off the camera rather than off the pose, so the basis is visible with
// no tracker connected and without loading a save, and again whenever the FOV
// being rendered moves away from what was last reported.
//
// THE GATE IS THAT THE FIRST LINE READS x1.0000.
void LogZoomBasis(const Viewport& viewport, float unzoomed4x3, float base, float live,
                  float factor, ZoomLog& log) {
    if (std::fabs(factor - log.logged_factor) <= kZoomLogBand) return;
    log.logged_factor = factor;
    HT_LOG("[view] head pose zoom factor x%.4f: rendering at %.2f horizontal degrees against "
           "an un-zoomed %.2f (%.1f at 4:3, widened by x%.4f for %dx%d) - %s",
           factor, live, base, unzoomed4x3, viewport.ratio, viewport.w, viewport.h,
           std::fabs(factor - kNoZoomScaling) > kZoomLogBand
               ? "the frame is not at the un-zoomed FOV, so yaw, pitch and lean are scaled "
                 "to displace the picture by as much as they would have been"
               : "the head pose is applied as it arrived");
}

// The factor the head pose scales by, from the FOV the frame is rendering at
// and the one an un-zoomed frame would.
//
// Both are CViewSetup::fov's own units - horizontal degrees, already widened for
// this viewport - because the base is built by putting the 4:3 reference through
// the same widening the engine applied to the live one. Same axis on both sides
// is the whole requirement: the widening is a tangent scaling, so it cancels out
// of the ratio, and the factor is the same number whether it is measured
// horizontally or vertically. Measuring the two sides on DIFFERENT axes does not
// cancel, and is a constant multiplier on the pose that nothing else would show.
float ZoomFactor(const Viewport& viewport, float unzoomed4x3, float live, ZoomLog& log) {
    if (!(unzoomed4x3 > 0.0f)) return kNoZoomScaling;
    if (!std::isfinite(live) || live <= 0.0f || live >= kMaxRenderableFov) {
        return kNoZoomScaling;
    }
    const float base = source::ScaleFovByWidthRatio(unzoomed4x3, viewport.ratio);
    if (!std::isfinite(base) || base <= 0.0f || base >= kMaxRenderableFov) {
        return kNoZoomScaling;
    }

    const float tanLive = TanHalf(live);
    const float tanBase = TanHalf(base);
    if (!(tanLive > 0.0f) || !(tanBase > 0.0f)) return kNoZoomScaling;

    const float factor = cameraunlock::camera::FovZoomFactor(tanLive, tanBase);
    if (!std::isfinite(factor) || factor <= 0.0f) return kNoZoomScaling;

    LogZoomBasis(viewport, unzoomed4x3, base, live, factor, log);
    return factor;
}

}  // namespace

void ResolveFovConVars(HMODULE client, const builds::BuildProfile& profile) {
    if (!profile.HasFovConVars()) {
        HT_LOG("[view] build profile has no FOV cvar address - the [View] Fov key is inert and "
               "the head pose is not scaled for zoom (head tracking is otherwise unaffected)");
        return;
    }
    const builds::FovConVarOffsets& off = profile.offsets.fov;
    const uint8_t* world = ResolveConVar(client, off, off.fov_desired_rva, "fov_desired");
    if (!world) return;

    const float value = ReadObfuscatedFloat(world, off.convar_value);
    // The decode is checked before it is trusted. fov_desired is a field of
    // view, so anything outside a plausible band means the value offset or the
    // obfuscation is not what this build does, and scaling every frame by a
    // ratio built from it would be worse than not offering the key at all.
    if (!std::isfinite(value) || value < 1.0f || value >= kMaxRenderableFov) {
        HT_LOG("[view] fov_desired decodes to %.4f, which is not a field of view - the "
               "[View] Fov key is inert this session and the head pose is not scaled for "
               "zoom; head tracking is otherwise unaffected", value);
        return;
    }

    g_fovDesiredObject = world;
    g_convarValueOffset = off.convar_value;
    HT_LOG("[view] FOV cvar resolved: fov_desired=%.1f", value);
}

// A viewport that does not read as a rect disables both jobs for the rest of the
// session rather than being retried every frame: it means the profile's
// CViewSetup offsets do not fit this client.dll, and a projection built from the
// NaN that follows is a black screen, not a cosmetic fault.
float PrepareFrameFov(const ViewSetup& view, float worldFov) {
    static bool s_disabled = false;
    if (s_disabled) return kNoZoomScaling;
    // Without fov_desired there is nothing to call un-zoomed, so there is no
    // override to apply and no factor to measure. ResolveFovConVars has already
    // said why.
    if (!g_fovDesiredObject) return kNoZoomScaling;

    const int w = view.RectWidth();
    const int h = view.RectHeight();
    if (w <= 0 || h <= 0) {
        s_disabled = true;
        HT_LOG("[view] FOV override and zoom compensation disabled: viewport reads as %dx%d - "
               "the build profile's CViewSetup offsets do not fit this client.dll", w, h);
        return kNoZoomScaling;
    }
    const Viewport viewport(w, h);

    // Read every frame rather than latched: the player can change fov_desired
    // from the console or the game's own slider mid-session, and both the
    // override and the zoom factor are defined relative to it.
    const float base = ReadObfuscatedFloat(g_fovDesiredObject, g_convarValueOffset);

    static FieldLog s_world;
    const bool overridden = ScaleField(view.Fov(), worldFov, base, viewport, "world", s_world);

    // What an un-zoomed frame renders at, which is the configured FOV exactly
    // when this frame went through the override and the player's own otherwise.
    // Reading it off the same call keeps the two in step through the frames the
    // override refuses.
    static ZoomLog s_zoom;
    return ZoomFactor(viewport, overridden ? worldFov : base, view.Fov(), s_zoom);
}

}  // namespace headtracking
