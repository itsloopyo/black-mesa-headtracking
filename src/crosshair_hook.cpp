// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#include "crosshair_hook.h"

#include <Windows.h>

#include <cstdint>
#include <vector>

#include "aim_point.h"
#include "builds/build_registry.h"
#include "debug_log.h"
#include "detour.h"

namespace headtracking {

namespace {

// CHudCrosshair::Paint is a plain __thiscall with no arguments; the vgui::ISurface
// methods are __thiscall too. A __fastcall detour takes `this` in ecx and a dummy
// edx, and forwards both, which is the standard x86 shape for hooking thiscall.
using PaintFn = void(__fastcall*)(void* self, void* edx);
using DrawLineFn = void(__fastcall*)(void* self, void* edx, int x0, int y0, int x1, int y1);
using DrawPolyLineFn = void(__fastcall*)(void* self, void* edx, int* px, int* py, int n);
using DrawOutlinedCircleFn = void(__fastcall*)(void* self, void* edx, int x, int y, int radius,
                                               int segments);

PaintFn              g_originalPaint = nullptr;
DrawLineFn           g_originalDrawLine = nullptr;
DrawPolyLineFn       g_originalDrawPolyLine = nullptr;
DrawOutlinedCircleFn g_originalDrawOutlinedCircle = nullptr;

// Render-thread only: Paint runs on the thread that paints the HUD, and every
// surface call inside it is made on that same thread before Paint returns.
struct Shift {
    bool active = false;
    int dx = 0;
    int dy = 0;
};
Shift g_shift;

enum class SurfaceState { Pending, Ready, Failed };
SurfaceState g_surfaceState = SurfaceState::Pending;

uintptr_t g_clientBase = 0;
const builds::AimOffsets* g_aim = nullptr;

// DrawPolyLine takes its points by pointer, so shifting them means drawing a
// shifted copy. Reused across frames: the RPG draws three points every frame.
std::vector<int> g_polyX;
std::vector<int> g_polyY;

void __fastcall Hook_DrawLine(void* self, void* edx, int x0, int y0, int x1, int y1) {
    if (g_shift.active) {
        x0 += g_shift.dx;
        x1 += g_shift.dx;
        y0 += g_shift.dy;
        y1 += g_shift.dy;
    }
    g_originalDrawLine(self, edx, x0, y0, x1, y1);
}

void __fastcall Hook_DrawPolyLine(void* self, void* edx, int* px, int* py, int n) {
    if (!g_shift.active || n <= 0) {
        g_originalDrawPolyLine(self, edx, px, py, n);
        return;
    }
    try {
        g_polyX.assign(px, px + n);
        g_polyY.assign(py, py + n);
    } catch (...) {
        // An exception unwinding out of a detour into the engine's HUD paint
        // ends the process. The unshifted line is the lesser fault.
        g_originalDrawPolyLine(self, edx, px, py, n);
        return;
    }
    for (int i = 0; i < n; ++i) {
        g_polyX[i] += g_shift.dx;
        g_polyY[i] += g_shift.dy;
    }
    g_originalDrawPolyLine(self, edx, g_polyX.data(), g_polyY.data(), n);
}

void __fastcall Hook_DrawOutlinedCircle(void* self, void* edx, int x, int y, int radius,
                                        int segments) {
    if (g_shift.active) {
        x += g_shift.dx;
        y += g_shift.dy;
    }
    g_originalDrawOutlinedCircle(self, edx, x, y, radius, segments);
}

// The surface methods are resolved through client.dll's own ISurface pointer,
// and the vftable slots are the ones its crosshair code calls. A wrong slot would
// be a detour on some other method, so each target has to land inside the
// module that implements the surface before anything is hooked.
void* SurfaceMethod(void** vtable, uint32_t slotBytes, HMODULE surfaceModule) {
    void* fn = vtable[slotBytes / sizeof(void*)];
    HMODULE owner = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(fn), &owner) ||
        owner != surfaceModule) {
        return nullptr;
    }
    return fn;
}

// Runs from the first Paint rather than from Install: client.dll's surface
// pointer is only filled in when the client initialises, which is after the ASI
// has installed its hooks - and by the time the crosshair paints, it has to be.
void InstallSurfaceHooks() {
    void* surface = *reinterpret_cast<void**>(g_clientBase + g_aim->surface_ptr_rva);
    if (!surface) return;  // not connected yet; the next Paint asks again

    g_surfaceState = SurfaceState::Failed;
    void** vtable = *reinterpret_cast<void***>(surface);

    HMODULE surfaceModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(vtable[0]), &surfaceModule)) {
        HT_LOG("[crosshair] the ISurface at %p has a vftable in no loaded module - the "
               "crosshair stays centred (head tracking is unaffected)", surface);
        return;
    }
    char surfaceName[MAX_PATH] = {};
    GetModuleFileNameA(surfaceModule, surfaceName, MAX_PATH);

    void* drawLine = SurfaceMethod(vtable, g_aim->slot_draw_line, surfaceModule);
    void* drawPolyLine = SurfaceMethod(vtable, g_aim->slot_draw_poly_line, surfaceModule);
    void* drawCircle = SurfaceMethod(vtable, g_aim->slot_draw_outlined_circle, surfaceModule);
    if (!drawLine || !drawPolyLine || !drawCircle) {
        HT_LOG("[crosshair] ISurface slots +0x%X/+0x%X/+0x%X do not all point into %s - the "
               "crosshair stays centred (head tracking is unaffected)",
               g_aim->slot_draw_line, g_aim->slot_draw_poly_line,
               g_aim->slot_draw_outlined_circle, surfaceName);
        return;
    }

    constexpr const char* kDormant = " - the crosshair stays centred (head tracking is unaffected)";
    // All three or none: a crosshair whose ticks move and whose circle does not
    // is worse than one that stays centred.
    if (!InstallDetour("crosshair", "ISurface::DrawLine", drawLine,
                       reinterpret_cast<void*>(&Hook_DrawLine),
                       reinterpret_cast<void**>(&g_originalDrawLine), kDormant) ||
        !InstallDetour("crosshair", "ISurface::DrawPolyLine", drawPolyLine,
                       reinterpret_cast<void*>(&Hook_DrawPolyLine),
                       reinterpret_cast<void**>(&g_originalDrawPolyLine), kDormant) ||
        !InstallDetour("crosshair", "ISurface::DrawOutlinedCircle", drawCircle,
                       reinterpret_cast<void*>(&Hook_DrawOutlinedCircle),
                       reinterpret_cast<void**>(&g_originalDrawOutlinedCircle), kDormant)) {
        return;
    }
    HT_LOG("[crosshair] surface draw calls hooked in %s", surfaceName);
    g_surfaceState = SurfaceState::Ready;
}

void __fastcall Hook_Paint(void* self, void* edx) {
    Shift shift;
    // Guarded for the same reason the render-view detour is (see
    // camera_hook.cpp): the correction logs through std::string, and an
    // exception unwinding out of a detour, through the trampoline and into
    // client.dll's HUD paint, ends the process. A frame drawn with the engine's
    // own centred crosshair does not.
    try {
        if (g_surfaceState == SurfaceState::Pending) InstallSurfaceHooks();
        bool behind = false;
        if (g_surfaceState == SurfaceState::Ready &&
            ComputeReticleOffset(shift.dx, shift.dy, behind)) {
            // A crosshair drawn anywhere while the shot is behind the view is a
            // crosshair that lies. Skipping all of Paint keeps its begin/end
            // pair balanced.
            if (behind) return;
            shift.active = true;
        }
    } catch (...) {
        // Deliberately silent: logging from here could throw again, and the
        // engine's own centred crosshair is the right thing to draw.
        shift = Shift{};
    }

    g_shift = shift;
    g_originalPaint(self, edx);
    g_shift = Shift{};
}

}  // namespace

bool CrosshairHook::Install() {
    // ResolveAimPoint is the gate on both the build profile and its aim
    // addresses, so a profile is guaranteed by the time it returns true.
    if (!ResolveAimPoint()) return false;
    const builds::BuildProfile* profile = builds::ActiveProfile();

    HMODULE client = GetModuleHandleA("client.dll");
    if (!client) {
        HT_LOG("[crosshair] client.dll not loaded");
        return false;
    }
    g_clientBase = reinterpret_cast<uintptr_t>(client);
    g_aim = &profile->offsets.aim;
    void* target = reinterpret_cast<void*>(g_clientBase + g_aim->crosshair_paint_rva);

    return InstallDetour("crosshair", "CHudCrosshair::Paint", target,
                         reinterpret_cast<void*>(&Hook_Paint),
                         reinterpret_cast<void**>(&g_originalPaint),
                         " - the crosshair stays centred (head tracking is unaffected)");
}

}  // namespace headtracking
