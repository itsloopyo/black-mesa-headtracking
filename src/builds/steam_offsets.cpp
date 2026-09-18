// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
// Steam Win32 build profiles for Black Mesa's client.dll. The install ships one
// client.dll, in bms\bin, and both the campaign and the deathmatch mode load
// it - so unlike the Half-Life 2 ancestor there is one profile per patch here,
// not one per campaign directory. Append-only: a new patch gets a new entry in
// this file and a new line at the TOP of kKnownProfiles in build_registry.cpp.
// Nothing in this file is ever edited in place, because editing a shipped
// profile strands every player who has not taken the patch yet.
//
// Everything below was read out of the running game rather than inferred from
// the Half-Life 2 profiles: the layout is Source's, but every address moved and
// the CViewSetup grew by 128 bytes.

#include "builds/build_registry.h"

namespace headtracking::builds {

// CViewSetup is 328 bytes on this branch - CViewRender::RenderView copies it as
// 0x52 dwords, twice, which is what fixes the size.
//
// The head of the struct is Source's and matches the ancestor's exactly:
// RenderView passes (+0x00, +0x08, +0x10, +0x18) to the viewport calls, so the
// rect ints are doubled (x/unscaledX, y/unscaledY, width/unscaledWidth), and it
// compares +0x1C against 2 and passes it as an eye index, which makes that
// field m_eStereoEye rather than the fourth unscaled int.
//
// The tail sits 0x7C further along than Half-Life 2's, and was pinned by
// measurement in game rather than by counting fields. At 1280x720, +0xB4 reads
// 106.2602, which is fov_desired's 90 widened by (1280/720)/(4/3) to the last
// decimal place, and +0xB8 reads 91.3085, which is 75 widened the same way.
// Watching a moving player fixed the rest: +0xBC..+0xC4 changed together as the
// player walked - (113.000, -1226.000, 584.112) to (256.082, -1236.380,
// 582.933) - while +0xC8..+0xD0 read (p0.000 y90.000 r0.000) turning to
// (p0.396 y98.910 r0.000). That is the only assignment that works: +0xCC swings
// past 130 and below -178, so it cannot be a pitch, which rules out the
// alternative of starting the origin at +0xC0. Everything after it lands where
// Source puts it - zNear 7.0 at +0xD4, zFar 30000.0 at +0xD8, zNearViewmodel
// 1.0 at +0xDC, zFarViewmodel 30000.0 at +0xE0, and the 1.7778 aspect at +0xE8.
constexpr ViewSetupOffsets kViewSetupLayout_20250607 = {
    0xBCu,  // origin (Vector x, y, z)
    0xC8u,  // angles (QAngle pitch, yaw, roll)
    0xB4u,  // fov, horizontal degrees, already widened for this viewport
    0xB8u,  // fovViewmodel - the float straight after fov
    0x10u,  // rect width
    0x18u,  // rect height
};

// The reticle surface, read off CHudCrosshair's own code rather than carried
// over from Half-Life 2, because Black Mesa draws its crosshair differently.
//
// CHudCrosshair::Paint is at 0x278460: it asks GetLocalPlayer (0xBA690, which
// returns the local-player global) for the active weapon, sets the draw colour
// through the surface at client.dll+0x70508C (vftable +0x34, DrawSetColor
// taking a Color), and calls the weapon's vftable +0x5C4. Every weapon class
// shares 0xA48D0 there, which forwards to +0x5CC - the per-weapon draw. Those
// draw functions (0x297000 for the base Black Mesa weapon, 0x290220 Glock,
// 0x291740 MP5, 0x28E490 .357, 0x2910D0 Hivehand, 0x291F80 RPG, 0x293430
// shotgun, 0x294610 Tau, 0x29B100 Gluon) centre on GetHudSize()/2 and draw
// through exactly three surface calls:
//
//   +0x48  DrawLine(x0, y0, x1, y1)         four ints; the radial ticks
//   +0x4C  DrawPolyLine(px, py, n)          pushed with n = 3; the RPG only
//   +0x1A0 DrawOutlinedCircle(x, y, r, 32)  shotgun, Tau and Gluon
//
// The only other surface call on that path is +0x38 with (0xFF, 0, 0, 0xFF),
// the four-int DrawSetColor, which has no coordinates to move.
//
// UTIL_TraceLine (0x78540) and trace_t come from the crosshair's enemy-detect
// think at 0x2782E0, which traces from the eye along the aim and reads fraction
// at +44 and m_pEnt at +76 off the result - CBaseTrace's layout, so endpos is
// at +12 as in the ancestor. The function builds a Ray from the two points,
// wraps the ignore entity and collision group in a trace filter and calls
// enginetrace->TraceRay, which is UTIL_TraceLine's body.
//
// ScreenTransform (0x1F9470) is the only small function that calls the
// engine's WorldToScreenMatrix (VEngineClient015 slot 36): it multiplies the
// point through rows 0, 1 and 3, returns 1 when w is behind the eye and
// divides by w otherwise. GetHudSize (0x1EB130) asks the surface for the
// screen size and is what the weapons' ScreenWidth / ScreenHeight wrappers
// read, so a pixel offset computed from it is in the same space they draw in.
constexpr AimOffsets kAimLayout_20250607 = {
    0x278460u,  // CHudCrosshair::Paint
    0x078540u,  // UTIL_TraceLine
    0x1F9470u,  // ScreenTransform
    0x1EB130u,  // GetHudSize(&w, &h)
    0x0BA690u,  // C_BasePlayer::GetLocalPlayer
    0x70508Cu,  // vgui::ISurface* global
    0x48u,      // ISurface::DrawLine
    0x4Cu,      // ISurface::DrawPolyLine
    0x1A0u,     // ISurface::DrawOutlinedCircle
    12u,        // trace_t::endpos
    44u,        // trace_t::fraction
};

// client.dll's own IVEngineClient*, at client.dll+0x5A48AC. Found by scanning
// the image for the pointer engine.dll's CreateInterface("VEngineClient015")
// returns, once the game was actually rendering - the scan finds nothing if it
// runs at module-load time, because the engine has not connected the client
// yet. CViewRender::RenderView calls through that same global, which is the
// independent confirmation that it is the client's engine interface and not
// some other copy of the pointer.
//
// The slot numbers were read off engine.dll rather than carried over from the
// ancestor's VEngineClient014, because this is VEngineClient015 and the two do
// not have to agree. Five of them are established:
//
//   26 IsInGame                 `return signon == 6`, the same global that
//   51 GetLevelName             gates GetLevelName's return on `signon > 1`
//   21 GetMaxClients            `return <int global>`, reads 1 in single player
//   28 IsDrawingLoadingImage    `return <bool global>`
//   85 IsPaused                 `mov ecx, &cl; jmp CClientState::IsPaused`
//
// IsInGame and GetLevelName are identified by what they do, and they carry the
// slot numbers the ancestor's VEngineClient014 has, so the interface prefix is
// unchanged at least that far - which is what makes 21 and 28, both below 51
// and both the right shape, trustworthy too.
//
// IsPaused is read off a call site rather than counted, and it is one past the
// ancestor's 84. CHudCrosshair::ShouldDraw at client.dll+0x278560 (slot 9 of
// the class's primary vftable at rva 0x490520, its only reference) calls slot 28
// and then, on the very next branch, slot 85, before decoding the crosshair
// ConVar - the `!IsDrawingLoadingImage() && !IsPaused() && crosshair.GetInt()`
// chain Source's own ShouldDraw has. engine.dll agrees: CEngineClient's slot 85
// loads the same client-state object whose +0x130 IsInGame reads as the signon
// state, and jumps to a member that tests the byte at +0x1A4 (m_bPaused) and
// then ORs in the other pause sources. Slot 84 is the thin forwarder into
// another interface that ruled out the ancestor's number.
//
// IsLevelMainMenuBackground is NOT established and is left at 0, so the gate
// skips it. A wrong slot number is an indirect call through an unrelated entry
// - a crash in the player's game, not a wrong answer - so it stays absent until
// it is read off a call site. The level-name test covers the same case, since
// Black Mesa's own bms\gameinfo.txt lists its menu maps as background01..NN.
constexpr EngineStateOffsets kEngineState_20250607 = {
    0x5A48ACu,
    "VEngineClient015",
    26u,  // IsInGame
    85u,  // IsPaused
    0u,   // IsLevelMainMenuBackground - not derived on this build
    28u,  // IsDrawingLoadingImage
    21u,  // GetMaxClients
    51u,  // GetLevelName
};

// The fov_desired ConVar object, at client.dll+0x6D9730. Located from its name
// string: the only dword in the image pointing at "fov_desired" is the object's
// m_pszName, and reading m_pszDefaultValue and m_pszString back off the same
// object gave "90" and "90", which both proves the layout and gives the number
// the rendered 106.2602 was checked against.
//
// The field offsets are the standard 32-bit MSVC ConCommandBase / ConVar
// layout - m_pszName at 0x0C, m_pParent at 0x1C, m_fValue at 0x2C - confirmed
// against this object: m_pParent reads back as the object's own address, and
// m_StringLength at 0x28 reads 3, which is strlen("90") + 1.
//
// m_fValue is NOT a plain float here. Black Mesa stores it XORed with the
// ConVar object's own address, and 0x6E389730 read off this object in a process
// where it sat at 0x2C8C9730 decodes to 0x42B40000, which is 90.0f exactly. See
// fov_override.cpp for the decode and the guard it is done behind.
//
// viewmodel_fov is 0 because this client.dll does not register it - the string
// does not appear anywhere in the image.
constexpr FovConVarOffsets kFovConVars_20250607 = {
    0x6D9730u,  // fov_desired
    0u,         // viewmodel_fov - not registered by this client.dll
    0x0Cu,      // ConCommandBase::m_pszName
    0x1Cu,      // ConVar::m_pParent
    0x2Cu,      // ConVar::m_fValue, XORed with the object's own address
};

// Black Mesa's player flashlight is its own deferred light (the
// __GBLightSpot_FlashLight materials), not Source's CFlashlightEffect - that
// class survives only as the base of the vehicle's CHeadlightEffect, whose
// constructor is its one caller. The player's flashlight update at 0xBC830
// builds the eye origin and EyeVectors and passes them, with the player, to
// 0x189570: __thiscall on the light renderer, (origin, player, forward, right,
// up), ending in `ret 0x14`. That function is the one that reads the
// gb_flashlight_* cvars and writes the light's position and direction, so it
// is the flashlight hook's target.

// bms\bin\client.dll dated 2025-06-07. CViewRender::RenderView is slot 6 of the
// CViewRender vftable at rva 0x465150, at rva 0x20EE40 - slot 5 of that same
// vftable is at 0x1F2940, which a telemetry marker inside it names
// "CViewRender::Render", so the two sit in the order the ancestor's do.
//
// The arity is not a guess: the detour has to pass (view, clearFlags,
// whatToDraw) through, and RenderView's own body reads all three - it tests
// whatToDraw's bits 0, 1 and 2 and passes clearFlags to the clear call. Nor is
// the call path: the hook is an inline detour on the function, not a vftable
// patch, because CViewRender::Render calls RenderView as a direct member call
// and a patched vftable slot is never consulted. That was measured - a vftable
// patch produced no calls at all across a full session.
extern const BuildProfile kSteamProfile_20250607 = {
    "steam-win32-20250607",
    { 0x684499C9u, 0x007BC000u, 0x00000000u },
    { 0x20EE40u, kViewSetupLayout_20250607, kAimLayout_20250607, kEngineState_20250607,
      kFovConVars_20250607, 0x189570u },
};

}  // namespace headtracking::builds
