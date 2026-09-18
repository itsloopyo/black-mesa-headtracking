// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <cstdint>

#include "cameraunlock/memory/pe_fingerprint.h"

namespace headtracking::builds {

// Byte offsets of the CViewSetup fields the render-view detour reads and
// writes. The mod compiles against no engine headers, so these are rederived
// per build and pinned to that build's fingerprint - see the registry below.
struct ViewSetupOffsets {
    uint32_t origin;         // Vector origin
    uint32_t angles;         // QAngle angles (pitch, yaw, roll)
    uint32_t fov;            // float fov, horizontal degrees
    uint32_t fov_viewmodel;  // float fovViewmodel
    uint32_t rect_width;     // int width, the rendered viewport
    uint32_t rect_height;    // int height
};

// The client.dll surface the reticle needs. Black Mesa does not draw its
// crosshair the way stock Source does: there is no GetDrawPosition to answer.
// CHudCrosshair::Paint sets the colour and hands off to the active weapon, and
// every weapon draws its own crosshair with DrawLine / DrawPolyLine /
// DrawOutlinedCircle centred on GetHudSize()/2. So the mod detours Paint and,
// for the duration of that one call, shifts those three surface calls by the
// offset between screen centre and the projected aim point.
//
// The trace and the projection are still the game's own: UTIL_TraceLine along
// the clean aim, and ScreenTransform, which reads the engine's world-to-screen
// matrix for the frame just rendered. The surface slots are byte offsets into
// the vgui::ISurface vftable client.dll calls through.
struct AimOffsets {
    uint32_t crosshair_paint_rva;   // CHudCrosshair::Paint, __thiscall(this)
    uint32_t trace_line_rva;        // UTIL_TraceLine(start, end, mask, ignore, group, trace)
    uint32_t screen_transform_rva;  // ScreenTransform(worldPoint, ndc) - nonzero = behind
    uint32_t hud_size_rva;          // GetHudSize(&width, &height)
    uint32_t local_player_rva;      // C_BasePlayer::GetLocalPlayer()
    uint32_t surface_ptr_rva;       // client.dll's vgui::ISurface* global
    uint32_t slot_draw_line;        // ISurface::DrawLine(x0, y0, x1, y1), vftable byte offset
    uint32_t slot_draw_poly_line;   // ISurface::DrawPolyLine(px, py, n)
    uint32_t slot_draw_outlined_circle;  // ISurface::DrawOutlinedCircle(x, y, radius, segments)
    uint32_t trace_endpos;          // byte offset of trace_t::endpos
    uint32_t trace_fraction;        // byte offset of trace_t::fraction
};

// The scratch buffer aim_point.cpp hands the engine's trace, and the bound the
// two trace_t field offsets above are read within. Wider than any trace_t this
// engine writes, so the engine's own write always fits; the OFFSETS are
// per-build data, rederived by hand for every profile, so they are checked
// against it at load rather than trusted. An out-of-range one would read past
// the end of a stack buffer, which is the one way a mistyped offset stops being
// a wrong crosshair and starts being a memory fault.
constexpr uint32_t kTraceResultBufferSize = 256;

// Written as a subtraction so an offset near the top of the range cannot wrap
// the addition and pass the check.
constexpr bool TraceFieldFits(uint32_t offset, uint32_t size) {
    return offset <= kTraceResultBufferSize && size <= kTraceResultBufferSize - offset;
}

constexpr bool TraceFieldsFitBuffer(const AimOffsets& aim) {
    return TraceFieldFits(aim.trace_endpos, static_cast<uint32_t>(sizeof(float) * 3))
        && TraceFieldFits(aim.trace_fraction, static_cast<uint32_t>(sizeof(float)));
}

// The ConVar the game bases its own field of view on. The mod needs it because
// the INI expresses its override in fov_desired's units, so turning that into a
// render-view FOV means knowing what the game is currently measuring from - and
// because the FOV in the render view is not always the player's: a suit zoom or
// a scripted camera writes its own, and an override that ignored that would
// flatten every one of them.
//
// `convar_name` is ConCommandBase::m_pszName and is there to be checked, not
// used: a ConVar object whose name reads back as the expected string is proof
// the rest of the layout fits, and three plausible floats are not.
//
// `convar_parent` matters here in a way it did not on stock Source. Black Mesa
// ships ConVar values OBFUSCATED: the dword at `convar_value` is the float's
// bits XORed with the ConVar object's own address, and the engine's own readers
// decode it that way inline (see fov_override.cpp). They only take that path
// when m_pParent is the object itself, and call the parent's virtual GetFloat
// otherwise, so the mod checks the same thing before decoding rather than
// XORing whatever it finds.
//
// `viewmodel_fov_rva` is 0 on every Black Mesa profile: this client.dll
// registers no viewmodel_fov ConVar at all, so there is nothing to express a
// viewmodel override against and the mod does not offer one.
struct FovConVarOffsets {
    uint32_t fov_desired_rva;    // the fov_desired ConVar object in client.dll
    uint32_t viewmodel_fov_rva;  // the viewmodel_fov ConVar object, 0 if absent
    uint32_t convar_name;        // byte offset of ConCommandBase::m_pszName
    uint32_t convar_parent;      // byte offset of ConVar::m_pParent
    uint32_t convar_value;       // byte offset of ConVar::m_fValue (obfuscated)
};

// The gameplay gate. `engine_ptr_rva` is client.dll's own IVEngineClient*, so
// the mod asks the same object the game does; the slot numbers below are that
// interface's, which is why the version string travels with them. Both are
// checked at load: the pointer must agree with engine.dll's CreateInterface for
// exactly this version, or the gate stays unresolved and the mod is dormant.
//
// A slot of 0 means "not derived on this build", and the gate skips that test
// rather than calling it. Slot 0 is a real method on every one of these
// interfaces, so it can never be a legitimate value for any field here - which
// is what makes it safe to overload as the absent marker. This is not a
// convenience: a slot number that has not been established for the running
// interface version is an indirect call through an entry meaning something
// else, and that is a crash in the player's game rather than a wrong answer.
// `slot_is_in_game` and `slot_get_level_name` are the two the gate cannot do
// without, and GameState::Resolve refuses a profile missing either.
struct EngineStateOffsets {
    uint32_t engine_ptr_rva;
    const char* interface_version;
    uint16_t slot_is_in_game;
    uint16_t slot_is_paused;
    uint16_t slot_is_menu_background;
    uint16_t slot_is_drawing_loading_image;
    uint16_t slot_get_max_clients;
    uint16_t slot_get_level_name;
};

// The whole surface one client.dll build pins.
struct OffsetTable {
    uint32_t render_view_rva;  // CViewRender::RenderView, RVA in client.dll
    ViewSetupOffsets view_setup;
    AimOffsets aim;
    EngineStateOffsets engine;
    FovConVarOffsets fov;
    uint32_t flashlight_update_rva;  // the light renderer's flashlight update
};

// One entry per shipped Black Mesa client.dll build we have offsets for. The
// PE fingerprint is the routing key.
struct BuildProfile {
    const char* name;
    cameraunlock::memory::PeFingerprint fingerprint;
    OffsetTable offsets;

    // A profile whose hook target is still unresolved is a placeholder: the
    // fingerprint of a build we have spotted but not yet rederived. It must
    // stay dormant rather than hook a stale address, so the entry can be
    // landed the moment a patch appears without risking a user's session.
    bool IsComplete() const { return offsets.render_view_rva != 0; }

    // Reticle compensation is a separate, optional surface: a profile can drive
    // the camera without it. A build whose aim addresses have not been derived
    // keeps head tracking and draws the vanilla centred crosshair.
    bool HasAimOffsets() const {
        const AimOffsets& a = offsets.aim;
        return a.crosshair_paint_rva != 0 && a.trace_line_rva != 0 &&
               a.screen_transform_rva != 0 && a.hud_size_rva != 0 &&
               a.local_player_rva != 0 && a.surface_ptr_rva != 0 &&
               a.slot_draw_line != 0 && a.slot_draw_poly_line != 0 &&
               a.slot_draw_outlined_circle != 0;
    }

    // The two slots the gate cannot be built without: without IsInGame it
    // cannot tell a level from the menu, and without GetLevelName it cannot
    // tell a level from the animated map the main menu runs behind itself.
    bool HasEngineState() const {
        return offsets.engine.engine_ptr_rva != 0 && offsets.engine.interface_version != nullptr &&
               offsets.engine.slot_is_in_game != 0 && offsets.engine.slot_get_level_name != 0;
    }

    // Also optional, and separately so: a build whose fov_desired ConVar has not
    // been located still tracks the head and still draws the reticle on the
    // shot, it just leaves the [View] Fov key inert.
    bool HasFovConVars() const { return offsets.fov.fov_desired_rva != 0; }
};

}  // namespace headtracking::builds
