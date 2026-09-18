// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "window_centering.h"

#include <Windows.h>
#include <cstdlib>

#include "cameraunlock/os/game_window.h"
#include "debug_log.h"

namespace headtracking {
namespace {

namespace os = cameraunlock::os;

// Matches the 60s game_state.cpp already allows the engine interface to appear,
// for the same reason: a cold start off a hard disk, on a thread where waiting
// costs the game nothing.
constexpr int   kPollAttempts   = 240;
constexpr DWORD kPollIntervalMs = 250;

// A window is only judged once its rect has held still this long. The engine
// shows the window before it has applied the video mode, so a decision taken on
// the first rect that appears can be about a size the player never sees, and is
// then undone by the engine's own move.
constexpr int kSettlePolls = 12;  // 3s of an unchanged rect.

void ForwardWindowLog(os::WindowLogLevel level, const char* message) {
    HT_LOG("[window] %s%s", level == os::WindowLogLevel::Warning ? "WARN: " : "", message);
}

int CenteredOrigin(int area_start, int area_extent, int window_extent) {
    return area_start + (area_extent - window_extent) / 2;
}

bool IsCenteredOn(const RECT& window, const RECT& area) {
    // The engine's own centring rounds the odd half-pixel up where the integer
    // maths here rounds it down, so an exact comparison would move the window
    // one pixel and report that as a fix.
    constexpr int kTolerance = 2;
    const int dx = window.left -
                   CenteredOrigin(area.left, area.right - area.left, window.right - window.left);
    const int dy = window.top -
                   CenteredOrigin(area.top, area.bottom - area.top, window.bottom - window.top);
    return std::abs(dx) <= kTolerance && std::abs(dy) <= kTolerance;
}

void CenterUnlessAlready(HWND window, const RECT& rect) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) {
        HT_LOG("[window] WARN: GetMonitorInfoW failed: %lu", GetLastError());
        return;
    }

    // Either reading counts as centred. Source centres a windowed-mode window
    // on the monitor, this mod centres on the work area, and the two differ by
    // half the taskbar. Moving a window the engine already centred trades a
    // visible jump for nothing, and a fullscreen or borderless window is
    // centred by definition, which is how it is left alone here.
    if (IsCenteredOn(rect, info.rcWork) || IsCenteredOn(rect, info.rcMonitor)) {
        HT_LOG("[window] %dx%d at (%d, %d) is already centred, leaving it alone",
               static_cast<int>(rect.right - rect.left),
               static_cast<int>(rect.bottom - rect.top), static_cast<int>(rect.left),
               static_cast<int>(rect.top));
        return;
    }

    os::CenterGameWindowOnce(&ForwardWindowLog);
}

// Waits for a game window whose rect has held still for kSettlePolls, writing it
// and that rect to the out-params. False when none settled in time, in which
// case the out-params are left alone.
bool WaitForSettledWindow(HWND& settled, RECT& settled_rect) {
    RECT previous{};
    bool have_previous = false;
    int stable_polls = 0;

    for (int attempt = 0; attempt < kPollAttempts; ++attempt) {
        Sleep(kPollIntervalMs);

        const HWND window = os::FindGameWindow();
        RECT current{};
        if (!window || !GetWindowRect(window, &current)) {
            have_previous = false;
            stable_polls = 0;
            continue;
        }

        if (have_previous && EqualRect(&previous, &current)) {
            if (++stable_polls < kSettlePolls) continue;
            settled = window;
            settled_rect = current;
            return true;
        }
        previous = current;
        have_previous = true;
        stable_polls = 0;
    }
    return false;
}

}  // namespace

void CenterWindowWhenReady() {
    HWND window = nullptr;
    RECT rect{};
    if (!WaitForSettledWindow(window, rect)) {
        HT_LOG("[window] no window settled within %ds, leaving placement alone",
               static_cast<int>(kPollAttempts * kPollIntervalMs / 1000));
        return;
    }
    CenterUnlessAlready(window, rect);
}

}  // namespace headtracking
