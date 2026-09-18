// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

namespace headtracking {

// Waits for the game to bring its window up and stop moving it, then centres it
// on the work area of the monitor it is already on. A window that already sits
// centred, and one that fills the work area, are both left alone - so the only
// thing this ever moves is an off-centre windowed-mode game: fullscreen and
// borderless both cover the whole monitor.
//
// Blocks for as long as the game takes to get there, so it is called last from
// the bootstrap thread and nothing the mod needs may wait behind it.
void CenterWindowWhenReady();

}  // namespace headtracking
