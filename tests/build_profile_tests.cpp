// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
// Tests for the build-profile failsafes (src/builds/build_profile.h) and the
// shipped Steam profile (src/builds/steam_offsets.cpp).
//
// Everything here guards a DORMANCY decision. A profile that reports itself
// complete when it is not gets a detour installed against a stale RVA, and a
// trace offset that does not fit the buffer it indexes is read from past the
// end of a stack array. Neither has an in-game symptom short of a crash, and
// neither is visible in a diff of the offsets themselves.

#include <cstdio>

#include "builds/build_registry.h"
#include "test_checks.h"

namespace {

using namespace headtracking::builds;

headtracking::tests::CheckCounter Check;

// Every profile the mod ships, newest first, mirroring kKnownProfiles. A new
// entry belongs here too - these are the checks that would catch a half-filled
// profile, and a profile nobody asserts on is a profile nobody checked.
const BuildProfile* const kShippedProfiles[] = {
    &kSteamProfile_20250607,
};

constexpr size_t kShippedCount = sizeof(kShippedProfiles) / sizeof(kShippedProfiles[0]);

void TestShippedSteamProfiles() {
    std::printf("shipped Steam profiles\n");

    for (const BuildProfile* p : kShippedProfiles) {
        std::printf("  %s\n", p->name);
        Check(p->IsComplete(), "carries a RenderView RVA");
        Check(p->HasEngineState(), "carries the gameplay gate");
        Check(p->HasFovConVars(), "carries the FOV cvar");
        Check(p->HasAimOffsets(), "carries the aim addresses");
        Check(p->offsets.flashlight_update_rva != 0, "carries the flashlight update");
        Check(TraceFieldsFitBuffer(p->offsets.aim),
              "its trace_t offsets are read inside the trace buffer");
    }
}

// The reticle surface is all-or-nothing. HasAimOffsets() checks the six
// addresses and three surface slots it needs, so a profile carrying some of
// them reports false and the rest look derived to a reader without being used - which is how a
// half-finished derivation survives a review. Either every address is present
// or none is.
void TestAimOffsetsAreAllOrNothing() {
    std::printf("aim offsets are all-or-nothing\n");

    for (const BuildProfile* p : kShippedProfiles) {
        const AimOffsets& a = p->offsets.aim;
        const bool anyPresent = a.crosshair_paint_rva != 0 || a.trace_line_rva != 0 ||
                                a.screen_transform_rva != 0 || a.hud_size_rva != 0 ||
                                a.local_player_rva != 0 || a.surface_ptr_rva != 0 ||
                                a.slot_draw_line != 0 || a.slot_draw_poly_line != 0 ||
                                a.slot_draw_outlined_circle != 0;
        Check(anyPresent == p->HasAimOffsets(), p->name);
    }
}

// Two gate fields holding the same slot number is a copy-paste, and it is
// invisible: the gate keeps working, it just asks the same question twice and
// never asks the other one. Zero is the "not derived" marker and is allowed to
// repeat.
void TestEngineSlotsAreDistinct() {
    std::printf("gameplay gate slots\n");

    for (const BuildProfile* p : kShippedProfiles) {
        const EngineStateOffsets& e = p->offsets.engine;
        const uint16_t slots[] = {
            e.slot_is_in_game,      e.slot_is_paused,       e.slot_is_menu_background,
            e.slot_is_drawing_loading_image, e.slot_get_max_clients, e.slot_get_level_name,
        };
        bool distinct = true;
        for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
            for (size_t j = i + 1; j < sizeof(slots) / sizeof(slots[0]); ++j) {
                if (slots[i] != 0 && slots[i] == slots[j]) distinct = false;
            }
        }
        Check(distinct, p->name);
        Check(e.interface_version != nullptr, "names the interface version its slots belong to");
    }
}

// Routing between profiles is decided purely by fingerprint. Two profiles
// sharing one would not fail to build, and would fail no check above -
// MatchProfile walks kKnownProfiles in order and returns the first hit, so the
// shadowed build would silently be hooked with the other's RVAs. Distinctness
// is the whole basis of the routing, and this is the check that has to be here
// before the second profile lands rather than after.
void TestProfileFingerprintsAreDistinct() {
    std::printf("fingerprint distinctness\n");

    if (kShippedCount < 2) {
        std::printf("  [PASS] only one profile is shipped, nothing to collide with\n");
        return;
    }
    for (size_t i = 0; i < kShippedCount; ++i) {
        for (size_t j = i + 1; j < kShippedCount; ++j) {
            Check(!kShippedProfiles[i]->fingerprint.Matches(kShippedProfiles[j]->fingerprint),
                  kShippedProfiles[j]->name);
        }
    }
}

void TestIncompleteProfileStaysDormant() {
    std::printf("dormancy failsafes\n");

    // A placeholder: the fingerprint of a build that has been spotted but whose
    // hook target has not been rederived. Landing one must not arm the detour.
    BuildProfile placeholder = kSteamProfile_20250607;
    placeholder.offsets.render_view_rva = 0;
    Check(!placeholder.IsComplete(), "a zero RenderView RVA reports incomplete");

    BuildProfile noGate = kSteamProfile_20250607;
    noGate.offsets.engine.engine_ptr_rva = 0;
    Check(!noGate.HasEngineState(), "a missing engine pointer disables the gameplay gate");

    // The two slots the gate is meaningless without. An optional slot going to
    // zero is a supported state; these two are not.
    BuildProfile noInGame = kSteamProfile_20250607;
    noInGame.offsets.engine.slot_is_in_game = 0;
    Check(!noInGame.HasEngineState(), "a missing IsInGame slot disables the gameplay gate");

    BuildProfile noLevelName = kSteamProfile_20250607;
    noLevelName.offsets.engine.slot_get_level_name = 0;
    Check(!noLevelName.HasEngineState(), "a missing GetLevelName slot disables the gate");

    BuildProfile noFov = kSteamProfile_20250607;
    noFov.offsets.fov.fov_desired_rva = 0;
    Check(!noFov.HasFovConVars(), "a missing cvar address disables the FOV override");

    // Black Mesa registers no viewmodel_fov, so the FOV surface has to stay
    // usable with that address absent. Requiring both - which is what the
    // Half-Life 2 ancestor does - would leave the [View] Fov key permanently
    // inert here for a reason no log line would explain.
    BuildProfile noViewmodel = kSteamProfile_20250607;
    noViewmodel.offsets.fov.viewmodel_fov_rva = 0;
    Check(noViewmodel.HasFovConVars(),
          "an absent viewmodel_fov does not disable the world FOV override");
}

void TestTraceOffsetsAreBoundsChecked() {
    std::printf("trace_t offset bounds\n");

    // endpos is three floats and fraction is one, so the last offset that fits
    // is the buffer size minus that field's own width.
    Check(TraceFieldFits(kTraceResultBufferSize - 12u, 12u), "the last in-range endpos fits");
    Check(!TraceFieldFits(kTraceResultBufferSize - 11u, 12u),
          "an endpos one byte past the end is rejected");
    Check(TraceFieldFits(kTraceResultBufferSize - 4u, 4u), "the last in-range fraction fits");
    Check(!TraceFieldFits(kTraceResultBufferSize, 4u),
          "an offset at the end of the buffer is rejected");

    // The check is a subtraction, not an addition, so an offset near the top of
    // the range cannot wrap past it and read as in-bounds.
    Check(!TraceFieldFits(0xFFFFFFF8u, 12u), "a wrap-around offset is rejected");

    BuildProfile overrun = kSteamProfile_20250607;
    overrun.offsets.aim.trace_endpos = kTraceResultBufferSize - 4u;
    Check(!TraceFieldsFitBuffer(overrun.offsets.aim),
          "a profile whose endpos runs past the buffer is refused");

    BuildProfile fractionOverrun = kSteamProfile_20250607;
    fractionOverrun.offsets.aim.trace_fraction = kTraceResultBufferSize + 4u;
    Check(!TraceFieldsFitBuffer(fractionOverrun.offsets.aim),
          "a profile whose fraction sits past the buffer is refused");
}

}  // namespace

int RunBuildProfileTests() {
    std::printf("\nBuild profiles\n==============\n");
    TestShippedSteamProfiles();
    TestAimOffsetsAreAllOrNothing();
    TestEngineSlotsAreDistinct();
    TestProfileFingerprintsAreDistinct();
    TestIncompleteProfileStaysDormant();
    TestTraceOffsetsAreBoundsChecked();
    return Check.failures();
}
