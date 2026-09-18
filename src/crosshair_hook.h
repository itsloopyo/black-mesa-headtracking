// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

namespace headtracking {

// Puts the crosshair back on the point the player is actually shooting at.
//
// Shots already fly along the clean mouse aim - the render-view hook never
// touches the player's own eye angles - but the crosshair is drawn at the centre
// of the HUD, which is the centre of the head-tracked frame. So the vanilla
// crosshair follows the head and stops marking the shot the moment the two
// cameras differ.
//
// Black Mesa has no single "where does the crosshair go" function to answer.
// CHudCrosshair::Paint hands off to the active weapon, and each weapon draws its
// own crosshair around the HUD centre with three vgui surface calls. So this
// detours Paint, works out the offset aim_point.cpp resolves, and shifts those
// three calls by it for the duration of that one Paint - every weapon keeps its
// own crosshair art, drawn where the shot lands. Outside Paint the surface
// detours pass everything through untouched.
//
// Installed only on a build profile carrying the aim addresses; without them
// the game keeps its vanilla centred crosshair and head tracking is unaffected.
class CrosshairHook {
public:
    CrosshairHook() = default;

    bool Install();
};

}  // namespace headtracking
