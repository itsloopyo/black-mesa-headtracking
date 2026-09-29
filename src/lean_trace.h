// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include "cameraunlock/camera/lean_clamp.h"

namespace headtracking {
namespace builds { struct BuildProfile; }

namespace lean_trace {

// The engine half of the lean collision clamp: "what is between the clean eye
// and where the lean wants to put it". The policy half is core's LeanClamp.
//
// A zero-extent ray through the game's own UTIL_TraceLine, with the standoff
// carried in the clamp rather than in a swept volume. A box as wide as the
// standoff would start solid wherever the eye already sits closer than that to
// a surface, which a crouch in one of Black Mesa's vents does to the ceiling,
// and a sweep that starts solid refuses the lean in every direction.

// The standoff the query holds the eye off a surface by, along the surface's
// normal. Passed as LeanClamp::Apply's context.
struct Context {
    float skin = 0.0f;
};

// Resolves the trace from the active profile. False leaves the lean unclamped,
// and says why in the log.
bool Resolve(const builds::BuildProfile& profile);

// LeanQueryFn. Unqueried when there is no local player or the trace result
// does not read back as one.
cameraunlock::camera::LeanObstruction Query(void* context, const cameraunlock::math::Vec3& start,
                                            const cameraunlock::math::Vec3& direction,
                                            float maxDistance);

// What the last blocking query hit: how far along the ray, and the cosine of
// the angle between the lean and the surface normal the standoff was divided by.
struct LastHit {
    float distance = 0.0f;
    float cosine = 0.0f;
};
LastHit Last();

}  // namespace lean_trace
}  // namespace headtracking
