// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <cmath>

#include "cameraunlock/camera/zoom_compensation.h"

namespace headtracking {

// Keeps head tracking worth the same distance across the screen whatever field
// of view the frame is being rendered at.
//
// A narrow field of view magnifies everything in the frame, head tracking
// included: the head still turns ten degrees and the camera still turns ten
// degrees, and the picture simply moves further, by the ratio between the FOV
// being rendered and the game's own un-zoomed one. Black Mesa narrows it for
// the crossbow's scope and for scripted cameras, and unscaled the player reads
// that as the mod's sensitivity jumping the moment they zoom.
//
// This is a boundary conversion in the same family as the axis signs in
// camera_hook.cpp, not a setting: it has no ini key, it does not reshape the
// tracker's pose, and the factor is exactly 1.0 through all of ordinary play.
// The factor itself comes from fov_override.cpp, which is the only thing that
// knows what an un-zoomed frame renders at (the player's fov_desired, widened
// for the viewport, and put through the [View] Fov override when that frame
// was). What is left here is spending it.
//
// Yaw and pitch TRANSLATE the picture across the frame, so both scale. A lean
// translates it too, and linearly, so position is a plain multiply at the call
// site rather than a function here. ROLL DOES NOT: ten degrees of head roll
// rolls the picture ten degrees at every field of view there is, so scaling it
// would flatten a tilt the player is holding and buy nothing. That split, and
// the arithmetic, are core's - cameraunlock/camera/zoom_compensation.h.

// The widest angle the tangent round trip is handed. It is a scaling only
// inside a quarter turn: tan is negative in the second quadrant while atan
// answers in the first, so an unguarded 100 degrees comes back as -80 and a
// head sweeping through vertical would flip the view half a turn between two
// frames.
constexpr float kMaxZoomScaledAngle = 89.0f;

// One rotation axis, rescaled so it displaces the picture by as much as it
// would have at the un-zoomed FOV. Degrees in, degrees out, in the TRACKER's
// convention: this runs before the engine signs, so what reaches the QAngle is
// still bounded the way the unscaled pose was.
//
// Past the limit the angle is SATURATED and the remainder added back, rather
// than the whole angle being passed through. Both branches agree exactly at the
// limit, so the function stays continuous and monotone; passing the far side
// through unscaled instead steps by 1.4 degrees at a 2.4x zoom, between two head
// positions a hundredth of a degree apart. There is no screen displacement left
// to preserve out there anyway - the view is already turned clear of the frame.
inline float ScaleRotationForZoom(float deg, float zoomFactor) {
    if (!std::isfinite(deg) || !(zoomFactor > 0.0f)) return deg;
    // Ordinary play, where the factor is exactly 1.0: the round trip is the
    // identity there to within float noise, and skipping it keeps the pose
    // bit-for-bit what the tracker sent.
    if (zoomFactor == 1.0f) return deg;

    const float saturated = deg > kMaxZoomScaledAngle    ?  kMaxZoomScaledAngle
                            : deg < -kMaxZoomScaledAngle ? -kMaxZoomScaledAngle
                                                         : deg;
    return cameraunlock::camera::ScaleAngleForZoom(saturated, zoomFactor) + (deg - saturated);
}

}  // namespace headtracking
