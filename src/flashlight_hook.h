// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

namespace headtracking {
namespace builds { struct BuildProfile; }
struct AimState;

// Points the player's flashlight along the head-tracked view, turned by
// [Light] LightMultiplier for each degree of head turn, from the eye the frame
// is drawn from, while [Light] LightFollowsHead is on. The weapon's aim is
// untouched: only the light moves.
//
// Black Mesa's flashlight is its own deferred light, not Source's
// CFlashlightEffect. C_BasePlayer's flashlight update hands the light renderer
// the eye origin and the three eye vectors each frame; the hook swaps those for
// the tracked ones when the player is the local one.
bool InstallFlashlightHook(void* client, const builds::BuildProfile& profile);

// Bracket the original RenderView call. Begin replays an update the player made
// earlier in the frame, so the beam and the camera consume the same pose.
void BeginFlashlightView(const AimState& aim);
void EndFlashlightView();

}  // namespace headtracking
