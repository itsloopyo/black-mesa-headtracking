// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "hotkey_handler.h"

#include "cameraunlock/input/chord_hotkeys.h"
#include "debug_log.h"
#include "hotkeys.h"
#include "plugin.h"

namespace headtracking {

void HotkeyHandler::Start(Plugin& plugin, int toggle_vk, int yaw_mode_vk,
                          int mode_cycle_vk) {
    using cameraunlock::input::ChordGuarded;
    using cameraunlock::input::NavGuarded;

    // Each action is one call and nothing else: the Plugin method owns both the
    // state change and the log line, so the nav key and its chord cannot drift
    // apart or report the transition twice.
    const auto toggle    = [&plugin]() { plugin.ToggleEnabled(); };
    const auto yawMode   = [&plugin]() { plugin.ToggleYawMode(); };
    const auto modeCycle = [&plugin]() { plugin.CycleTrackingMode(); };

    // Nav-cluster bindings are suppressed while Ctrl+Shift is held so the chord
    // path is the sole trigger and a remapped nav key cannot fire twice.
    m_poller.SetToggleKey(toggle_vk, NavGuarded(toggle));
    m_poller.AddHotkey(yaw_mode_vk, NavGuarded(yawMode));
    m_poller.AddHotkey(mode_cycle_vk, NavGuarded(modeCycle));

    m_poller.AddHotkey(hotkeys::kVkY, ChordGuarded(toggle));
    m_poller.AddHotkey(hotkeys::kVkH, ChordGuarded(yawMode));
    m_poller.AddHotkey(hotkeys::kVkG, ChordGuarded(modeCycle));

    // Start() reports a poller that has no thread behind it. Dropping that
    // answer leaves every key in this function dead with nothing in the log to
    // say so, which reads to the player exactly like a mod that did not load.
    if (!m_poller.Start(kPollIntervalMs)) {
        HT_LOG("[hotkeys] the polling thread did not start - none of the keys respond this "
               "session; tracking runs on whatever [Network] EnableOnStartup selected");
    }
}

}  // namespace headtracking
