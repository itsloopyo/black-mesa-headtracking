// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "lean_trace.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "builds/build_profile.h"
#include "debug_log.h"

namespace headtracking::lean_trace {

namespace {

// Same signature as the aim's: void UTIL_TraceLine(start, end, mask, ignore, collisionGroup, trace).
using TraceLineFn = void(__cdecl*)(const float*, const float*, uint32_t, const void*, int, void*);
using LocalPlayerFn = void*(__cdecl*)();

TraceLineFn   g_traceLine = nullptr;
LocalPlayerFn g_localPlayer = nullptr;
uint32_t      g_fraction = 0;
uint32_t      g_normal = 0;
uint32_t      g_startSolid = 0;

// MASK_SOLID: world brushes, windows, moveables, grates and NPCs. Not
// MASK_PLAYERSOLID, whose player clips are invisible - a lean stopped by one
// stops in mid-air with nothing on screen to explain it.
constexpr uint32_t kMaskSolid = 0x0200400Bu;
constexpr int kCollisionGroupNone = 0;

// A wall met at a glancing angle has to be traced further along the lean to
// hold the eye `skin` off it, by skin / cos(angle), which runs to infinity at
// grazing incidence. Flooring the cosine bounds the overreach at skin / 0.2.
constexpr float kMinCosine = 0.2f;

// How far a surface normal's squared length may sit from 1 and still be read
// as a normal.
constexpr float kUnitTolerance = 0.02f;

LastHit g_last;

}  // namespace

bool Resolve(const builds::BuildProfile& profile) {
    if (!profile.HasLeanTrace()) {
        HT_LOG("[lean] build profile '%s' has no lean trace - leaning is not stopped by walls",
               profile.name);
        return false;
    }
    if (!builds::TraceFieldsFitBuffer(profile.offsets.aim) ||
        !builds::LeanTraceFieldsFitBuffer(profile.offsets.lean)) {
        HT_LOG("[lean] build profile '%s' puts a trace_t field outside the %u-byte trace buffer - "
               "leaning is not stopped by walls", profile.name, builds::kTraceResultBufferSize);
        return false;
    }
    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        HT_LOG("[lean] client.dll not loaded - leaning is not stopped by walls");
        return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(client);
    g_traceLine = reinterpret_cast<TraceLineFn>(base + profile.offsets.aim.trace_line_rva);
    g_localPlayer = reinterpret_cast<LocalPlayerFn>(base + profile.offsets.aim.local_player_rva);
    g_fraction = profile.offsets.aim.trace_fraction;
    g_normal = profile.offsets.lean.trace_plane_normal;
    g_startSolid = profile.offsets.lean.trace_start_solid;
    return true;
}

cameraunlock::camera::LeanObstruction Query(void* context, const cameraunlock::math::Vec3& start,
                                            const cameraunlock::math::Vec3& direction,
                                            float maxDistance) {
    cameraunlock::camera::LeanObstruction result;
    void* player = g_localPlayer();
    if (!player) return result;

    // The clamp asks for the lean plus its skin along the lean. The skin has to
    // hold along the surface normal instead, so trace far enough to find any
    // surface that could need it.
    const float skin = static_cast<const Context*>(context)->skin;
    const float lean = maxDistance - skin;
    const float length = lean + skin / kMinCosine;

    const float s[3] = {start.x, start.y, start.z};
    const float e[3] = {start.x + direction.x * length, start.y + direction.y * length,
                        start.z + direction.z * length};
    alignas(16) uint8_t trace[builds::kTraceResultBufferSize];
    std::memset(trace, 0, sizeof(trace));
    g_traceLine(s, e, kMaskSolid, player, kCollisionGroupNone, trace);

    float fraction = 0.0f;
    float normal[3] = {};
    std::memcpy(&fraction, trace + g_fraction, sizeof(fraction));
    std::memcpy(normal, trace + g_normal, sizeof(normal));
    if (!std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f) return result;

    result.queried = true;
    if (trace[g_startSolid] != 0) {
        // The eye the game put the camera at is already inside something, so
        // there is no room to lean anywhere.
        result.blocked = true;
        result.distance = 0.0f;
        g_last = {0.0f, 1.0f};
        return result;
    }
    if (fraction >= 1.0f) return result;

    const float distance = fraction * length;
    // A normal that does not read back as one floors to the widest standoff,
    // which stops the lean short - the safe direction to be wrong in.
    const float normalSq = normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2];
    float cosine = std::fabs(direction.x * normal[0] + direction.y * normal[1] +
                             direction.z * normal[2]);
    if (!(std::fabs(normalSq - 1.0f) <= kUnitTolerance) || !(cosine >= kMinCosine)) {
        cosine = kMinCosine;
    }
    if (cosine > 1.0f) cosine = 1.0f;

    // Reported so that the clamp's own (distance - skin) is the room left once
    // the eye is held `skin` off the surface along its normal.
    const float room = distance - skin / cosine;
    if (room >= lean) return result;
    result.blocked = true;
    result.distance = room + skin;
    g_last = {distance, cosine};
    return result;
}

LastHit Last() { return g_last; }

}  // namespace headtracking::lean_trace
