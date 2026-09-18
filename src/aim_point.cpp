// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "aim_point.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "aim_state.h"
#include "builds/build_registry.h"
#include "debug_log.h"
#include "log_throttle.h"
#include "source_math.h"

namespace headtracking {

namespace {

// void UTIL_TraceLine(const Vector& start, const Vector& end, unsigned mask,
//                     const IHandleEntity* ignore, int collisionGroup, trace_t* out)
using TraceLineFn = void(__cdecl*)(const float*, const float*, uint32_t, const void*, int, void*);
// int ScreenTransform(const Vector& point, Vector& screen) - nonzero when the
// point is behind the rendered view. screen is NDC: x right, y up, both -1..1.
using ScreenTransformFn = int(__cdecl*)(const float*, float*);
using HudSizeFn = void(__cdecl*)(int*, int*);
using LocalPlayerFn = void*(__cdecl*)();

TraceLineFn       g_traceLine = nullptr;
ScreenTransformFn g_screenTransform = nullptr;
HudSizeFn         g_hudSize = nullptr;
LocalPlayerFn     g_localPlayer = nullptr;
uint32_t          g_traceEndpos = 0;
uint32_t          g_traceFraction = 0;
bool              g_available = false;

// MASK_SHOT: solid world, windows, moveables, NPCs, debris and hitboxes - the
// stock Source value, which this client.dll pushes as an immediate at 24 call
// sites, so bullets are traced with it here too.
constexpr uint32_t kMaskShot = 0x46004003u;

// Source's MAX_TRACE_LENGTH, the map's own diagonal. Far enough that the far
// end of an unobstructed ray projects to the same pixel a true infinity would.
constexpr float kMaxTraceLength = 56755.84f;

// Under this the contact is the player's own body or a surface they are standing
// inside, and its parallax term would swing the crosshair across the screen. The
// ray's far end is used instead, which is the same answer a clear shot gets.
constexpr float kMinUsefulHitUnits = 8.0f;

// trace_t must report a point on the ray it was given. Checking that at load is
// what separates "the trace ran" from "the offsets in the profile are right":
// a wrong endpos offset still yields three plausible-looking floats.
constexpr float kEndposTolerance = 1.0f;

// Dense at first (the opening frames, then ~every 600), because that is where an
// offset fault shows; then one line per ~2000 frames, which is what a "the
// crosshair drifts at range" report is read from. Same schedule as the render
// hook's, so the two are the only steady writers - see log_throttle.h.
constexpr int kBurstLines           = 4;
constexpr int kEarlyLines           = 20;
constexpr int kEarlyIntervalFrames  = 600;
constexpr int kSteadyIntervalFrames = 2000;

struct TraceResult {
    float point[3];
    float distance;
    bool hit;
};

// Runs the game's own trace and reads back the two fields the reticle needs.
// Returns false only when the result is not a point on the ray, which means the
// profile's trace_t offsets do not fit this build.
bool TraceAim(const float start[3], const float dir[3], TraceResult& out) {
    const float end[3] = { start[0] + dir[0] * kMaxTraceLength,
                           start[1] + dir[1] * kMaxTraceLength,
                           start[2] + dir[2] * kMaxTraceLength };

    // Wider than any trace_t this engine writes, zeroed, and 16-byte aligned so
    // the vectors inside it land where the engine expects them. ResolveAimPoint
    // has already checked that both field offsets read back inside it.
    alignas(16) uint8_t result[builds::kTraceResultBufferSize];
    std::memset(result, 0, sizeof(result));
    g_traceLine(start, end, kMaskShot, g_localPlayer(), 0, result);

    float fraction = 0.0f;
    float endpos[3] = {};
    std::memcpy(&fraction, result + g_traceFraction, sizeof(fraction));
    std::memcpy(endpos, result + g_traceEndpos, sizeof(endpos));
    if (!std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f) return false;

    for (int i = 0; i < 3; ++i) {
        const float expected = start[i] + (end[i] - start[i]) * fraction;
        if (!std::isfinite(endpos[i]) || std::fabs(endpos[i] - expected) > kEndposTolerance) {
            return false;
        }
    }

    const float d = fraction * kMaxTraceLength;
    out.hit = fraction < 1.0f && d >= kMinUsefulHitUnits;
    out.distance = out.hit ? d : kMaxTraceLength;
    for (int i = 0; i < 3; ++i) out.point[i] = out.hit ? endpos[i] : end[i];
    return true;
}

void LogAim(const AimState& aim, const TraceResult& trace, const float ndc[2], bool behind,
            int dx, int dy, int w, int h) {
    static LogThrottle s_throttle(kBurstLines, kEarlyLines, kEarlyIntervalFrames,
                                  kSteadyIntervalFrames);
    if (!s_throttle.ShouldLog()) return;

    // One line, one frame: the distance, the lean that the parallax is
    // proportional to, and the pixel offset it produced. Reading those off
    // separate lines is how a distance fault gets mistaken for a sign fault.
    const float lean[3] = { aim.render_origin[0] - aim.clean_origin[0],
                            aim.render_origin[1] - aim.clean_origin[1],
                            aim.render_origin[2] - aim.clean_origin[2] };
    HT_LOG("[aim] dist=%.1f (%s) lean=(%.2f,%.2f,%.2f) clean=(p%.2f y%.2f) "
           "render=(p%.2f y%.2f r%.2f) ndc=(%.4f,%.4f)%s offset=(%d,%d) of %dx%d",
           trace.distance, trace.hit ? "hit" : "no hit", lean[0], lean[1], lean[2],
           aim.clean_angles[0], aim.clean_angles[1], aim.render_angles[0],
           aim.render_angles[1], aim.render_angles[2], ndc[0], ndc[1],
           behind ? " BEHIND" : "", dx, dy, w, h);
}

}  // namespace

bool ResolveAimPoint() {
    const builds::BuildProfile* profile = builds::ActiveProfile();
    if (!profile || !profile->HasAimOffsets()) {
        HT_LOG("[aim] build profile has no aim addresses - the crosshair stays centred "
               "(head tracking is unaffected)");
        return false;
    }

    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) return false;
    const auto base = reinterpret_cast<uintptr_t>(client);
    const builds::AimOffsets& off = profile->offsets.aim;

    // The two trace_t offsets index a fixed-size stack buffer, so they are a
    // bound to check, not a value to trust: they are rederived by hand for
    // every build profile, and a mistyped one would otherwise be read from past
    // the end of that buffer on the first frame the crosshair is drawn.
    if (!builds::TraceFieldsFitBuffer(off)) {
        HT_LOG("[aim] build profile '%s' puts trace_t::endpos (%u) or ::fraction (%u) outside "
               "the %u-byte trace buffer - the crosshair stays centred (head tracking is "
               "unaffected)",
               profile->name, off.trace_endpos, off.trace_fraction,
               builds::kTraceResultBufferSize);
        return false;
    }

    g_traceLine = reinterpret_cast<TraceLineFn>(base + off.trace_line_rva);
    g_screenTransform = reinterpret_cast<ScreenTransformFn>(base + off.screen_transform_rva);
    g_hudSize = reinterpret_cast<HudSizeFn>(base + off.hud_size_rva);
    g_localPlayer = reinterpret_cast<LocalPlayerFn>(base + off.local_player_rva);
    g_traceEndpos = off.trace_endpos;
    g_traceFraction = off.trace_fraction;
    g_available = true;
    return true;
}

bool ComputeReticleOffset(int& dx, int& dy, bool& behindCamera) {
    if (!g_available) return false;
    const AimState& aim = CurrentAimState();
    if (!aim.applied) return false;

    // No local player means no shot to mark - the crosshair is not drawn then
    // either, but the trace would run against a null ignore-entity.
    if (!g_localPlayer()) return false;

    float fwd[3], right[3], up[3];
    source::AngleVectors(aim.clean_angles, fwd, right, up);

    TraceResult trace;
    if (!TraceAim(aim.clean_origin, fwd, trace)) {
        g_available = false;
        HT_LOG("[aim] the trace did not report a point on the ray it was given - the profile's "
               "trace_t offsets do not fit this client.dll. Crosshair compensation is off for "
               "this session; head tracking is unaffected.");
        return false;
    }

    float ndc[3] = {};
    behindCamera = g_screenTransform(trace.point, ndc) != 0;

    int w = 0, h = 0;
    g_hudSize(&w, &h);
    if (w <= 0 || h <= 0) return false;

    // NDC runs y up and the HUD runs y down. Relative to the centre the weapons
    // draw around, that is the whole of the NDC-to-pixel mapping.
    const float fx = ndc[0] * 0.5f * static_cast<float>(w);
    const float fy = -ndc[1] * 0.5f * static_cast<float>(h);
    if (!std::isfinite(fx) || !std::isfinite(fy)) return false;
    dx = static_cast<int>(std::lround(fx));
    dy = static_cast<int>(std::lround(fy));

    LogAim(aim, trace, ndc, behindCamera, dx, dy, w, h);
    return true;
}

}  // namespace headtracking
