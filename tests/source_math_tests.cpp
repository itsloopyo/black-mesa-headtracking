// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
// Characterization tests for the Source Engine camera maths the render-view
// detour applies (src/source_math.cpp).
//
// These lock BEHAVIOUR, not a derivation: the expected values were captured
// from the shipped implementation, verified in-game against `getpos` on the
// client.dll the steam-win32-20250607 profile pins. A change that moves any of
// them changes what the player sees, and no in-game playtest catches a
// combined-axis drift automatically.

#include <cstdio>

#include "source_math.h"
#include "test_checks.h"

namespace {

using namespace headtracking::source;
using headtracking::tests::NearEqual;

headtracking::tests::CheckCounter Check;

void CheckTriple(const float actual[3], float e0, float e1, float e2, const char* name,
                 float eps = 1e-3f) {
    const bool ok = NearEqual(actual[0], e0, eps) && NearEqual(actual[1], e1, eps)
                    && NearEqual(actual[2], e2, eps);
    if (!ok) {
        std::printf("    expected (%.6g, %.6g, %.6g) got (%.6g, %.6g, %.6g)\n", e0, e1, e2,
                    actual[0], actual[1], actual[2]);
    }
    Check(ok, name);
}

// The width ratio for a viewport, exactly as fov_override.cpp computes it.
float WidthRatio(int w, int h) {
    return (static_cast<float>(w) / static_cast<float>(h)) * kReferenceAspectInverse;
}

void TestScaleFovByWidthRatio() {
    std::printf("ScaleFovByWidthRatio\n");
    Check(NearEqual(ScaleFovByWidthRatio(90.0f, 1.0f), 90.0f), "4:3 viewport leaves FOV alone");
    // Measured in game: fov_desired 90 in a 1280x800 window arrives at
    // RenderView as 100.39.
    Check(NearEqual(ScaleFovByWidthRatio(90.0f, WidthRatio(1280, 800)), 100.388855f),
          "90 at 1280x800 widens to 100.39");
    Check(NearEqual(ScaleFovByWidthRatio(90.0f, WidthRatio(1920, 1080)), 106.260201f),
          "90 at 16:9 widens to 106.26");
    Check(NearEqual(ScaleFovByWidthRatio(75.0f, WidthRatio(1280, 800)), 85.2772598f),
          "75 at 1280x800 widens to 85.28");
    Check(NearEqual(ScaleFovByWidthRatio(120.0f, WidthRatio(1280, 800)), 128.613235f),
          "120 at 1280x800 widens to 128.61");
    // Ultra-wide: still finite and below the 179 degree refusal threshold.
    Check(ScaleFovByWidthRatio(120.0f, WidthRatio(1280, 400)) < 179.0f,
          "120 on a 32:10 tile stays renderable");
}

void TestAngleVectors() {
    std::printf("AngleVectors\n");
    float fwd[3], right[3], up[3];

    const float identity[3] = { 0.0f, 0.0f, 0.0f };
    AngleVectors(identity, fwd, right, up);
    CheckTriple(fwd, 1.0f, 0.0f, 0.0f, "identity forward is +x");
    CheckTriple(right, 0.0f, -1.0f, 0.0f, "identity right is -y (Source is y-left)");
    CheckTriple(up, 0.0f, 0.0f, 1.0f, "identity up is +z");

    const float yaw90[3] = { 0.0f, 90.0f, 0.0f };
    AngleVectors(yaw90, fwd, right, up);
    CheckTriple(fwd, 0.0f, 1.0f, 0.0f, "yaw 90 forward is +y");
    CheckTriple(right, 1.0f, 0.0f, 0.0f, "yaw 90 right is +x");

    // Positive pitch looks DOWN in Source, so forward's z goes negative.
    const float pitch30[3] = { 30.0f, 0.0f, 0.0f };
    AngleVectors(pitch30, fwd, right, up);
    CheckTriple(fwd, 0.866025388f, 0.0f, -0.5f, "pitch 30 tilts forward downward");
    CheckTriple(up, 0.5f, 0.0f, 0.866025388f, "pitch 30 tilts up forward");

    const float roll45[3] = { 0.0f, 0.0f, 45.0f };
    AngleVectors(roll45, fwd, right, up);
    CheckTriple(fwd, 1.0f, 0.0f, 0.0f, "roll leaves forward alone");
    CheckTriple(right, 0.0f, -0.707106769f, -0.707106769f, "roll 45 tilts right");
    CheckTriple(up, 0.0f, -0.707106769f, 0.707106769f, "roll 45 tilts up");

    const float combined[3] = { 15.0f, 90.0f, 10.0f };
    AngleVectors(combined, fwd, right, up);
    CheckTriple(fwd, 0.0f, 0.965925813f, -0.258819044f, "combined pose forward");
    CheckTriple(right, 0.98480773f, -0.0449434109f, -0.167731255f, "combined pose right");
    CheckTriple(up, 0.173648164f, 0.254886985f, 0.951251209f, "combined pose up");
}

// Runs a QAngle out to a basis and back, which is what the camera-local yaw
// path does either side of its change of basis.
void RoundTrip(float pitch, float yaw, float roll, float e0, float e1, float e2,
               const char* name) {
    const float ang[3] = { pitch, yaw, roll };
    float fwd[3], right[3], up[3];
    AngleVectors(ang, fwd, right, up);
    const float left[3] = { -right[0], -right[1], -right[2] };
    float out[3];
    BasisToAngles(fwd, left, up, out);
    CheckTriple(out, e0, e1, e2, name);
}

void TestBasisToAngles() {
    std::printf("BasisToAngles\n");
    RoundTrip(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, "identity round-trips");
    RoundTrip(30.0f, 0.0f, 0.0f, 30.0f, 0.0f, 0.0f, "pitch round-trips");
    RoundTrip(0.0f, 90.0f, 0.0f, 0.0f, 90.0f, 0.0f, "yaw round-trips");
    RoundTrip(0.0f, 0.0f, 45.0f, 0.0f, 0.0f, 45.0f, "roll round-trips");
    RoundTrip(15.0f, 90.0f, 10.0f, 15.0f, 90.0f, 10.0f, "combined pose round-trips");
    RoundTrip(-89.9f, 45.0f, 0.0f, -89.9f, 45.0f, 0.0f,
              "just short of vertical stays on the normal branch");

    // Straight up / down: yaw and roll are the same axis, so Source folds the
    // whole rotation into yaw and zeroes roll. Yaw 30 + roll 20 comes back as
    // yaw 50.
    RoundTrip(90.0f, 45.0f, 0.0f, 90.0f, 45.0f, 0.0f, "straight down folds into yaw");
    RoundTrip(-90.0f, 30.0f, 20.0f, -90.0f, 50.0f, 0.0f,
              "straight up folds roll into yaw and zeroes roll");
}

void CheckCameraLocal(float basePitch, float baseYaw, float baseRoll, float dpitch, float dyaw,
                      float droll, float e0, float e1, float e2, const char* name) {
    float ang[3] = { basePitch, baseYaw, baseRoll };
    ApplyCameraLocalRotation(ang, dpitch, dyaw, droll);
    CheckTriple(ang, e0, e1, e2, name);
}

void TestApplyCameraLocalRotation() {
    std::printf("ApplyCameraLocalRotation\n");
    CheckCameraLocal(0, 0, 0, 0, 0, 0, 0, 0, 0, "zero delta on identity is a no-op");
    CheckCameraLocal(30, 45, 0, 0, 0, 0, 30, 45, 0, "zero delta preserves the base pose");

    // At the horizon the camera's own axes line up with the world's, so a
    // camera-local delta lands exactly where the world-space path would.
    CheckCameraLocal(0, 90, 0, 0, 25, 0, 0, 115, 0, "level camera: yaw adds like world yaw");
    CheckCameraLocal(0, 90, 0, 15, 0, 0, 15, 90, 0, "level camera: pitch adds");
    CheckCameraLocal(0, 90, 0, 0, 0, 10, 0, 90, 10, "level camera: roll adds");

    // Steeply pitched, which is the case the world-space path gets wrong: the
    // delta is composed about the camera's axes, so it feeds back into all
    // three angles. These values were checked by hand against the change of
    // basis.
    CheckCameraLocal(70, 90, 0, 0, 25, 0, 58.3916664f, 143.741272f, 49.26408f,
                     "steep pitch: yaw spreads across pitch, yaw and roll");
    CheckCameraLocal(70, 90, 0, 15, 0, 0, 85.0f, 90.0f, 0.0f,
                     "steep pitch: pitch still adds straight on");
    CheckCameraLocal(30, 45, 0, 15, 25, 10, 41.4416428f, 77.9945374f, 26.3727093f,
                     "moderate pitch with a combined delta");
    CheckCameraLocal(-60, 180, 0, -20, -30, -5, -61.1373672f, 103.25769f, 58.7720413f,
                     "negative pitch with a combined delta");
}

// The lean is projected onto this basis - camera_hook.cpp adds
// right*x + up*y + fwd*z to the render origin - so a basis that is not
// orthonormal moves the eye a different distance than the tracker asked for,
// along an axis that is not quite the one it names. Neither shows up as
// anything but "leaning feels off", including at the vertical, where the
// formula's sines and cosines collapse onto each other.
void TestAngleVectorsStayOrthonormal() {
    std::printf("AngleVectors orthonormality\n");

    const float pitches[] = { -90.0f, -89.9f, -45.0f, 0.0f, 45.0f, 89.9f, 90.0f };
    const float yaws[]    = { -180.0f, -90.0f, 0.0f, 37.0f, 179.9f };
    const float rolls[]   = { -180.0f, -45.0f, 0.0f, 45.0f, 180.0f };

    const auto dot = [](const float a[3], const float b[3]) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };

    for (float p : pitches) {
        for (float y : yaws) {
            for (float r : rolls) {
                const float ang[3] = { p, y, r };
                float fwd[3], right[3], up[3];
                AngleVectors(ang, fwd, right, up);
                const bool ok =
                    NearEqual(dot(fwd, fwd), 1.0f, 1e-4f) &&
                    NearEqual(dot(right, right), 1.0f, 1e-4f) &&
                    NearEqual(dot(up, up), 1.0f, 1e-4f) &&
                    NearEqual(dot(fwd, right), 0.0f, 1e-4f) &&
                    NearEqual(dot(fwd, up), 0.0f, 1e-4f) &&
                    NearEqual(dot(right, up), 0.0f, 1e-4f);
                if (!ok) {
                    std::printf("    (p%.1f y%.1f r%.1f) produced a skewed basis\n", p, y, r);
                    Check(false, "every pose gives unit, mutually perpendicular axes");
                    return;
                }
            }
        }
    }
    Check(true, "every pose gives unit, mutually perpendicular axes");
}

// What the camera-local path hands the engine. A QAngle out of range is a
// wrong camera; a non-finite one is a projection the renderer cannot build at
// all, which is a black screen rather than a cosmetic fault. The composition
// runs through a change of basis and atan2, and BasisToAngles switches branch
// at the vertical, so the poses either side of that pole are the ones worth
// pinning - a straight-down camera is reachable in one mouse flick.
void TestCameraLocalCompositionStaysRenderable() {
    std::printf("ApplyCameraLocalRotation output range\n");

    const float basePitches[] = { -90.0f, -89.999f, -60.0f, 0.0f, 60.0f, 89.999f, 90.0f };
    const float baseYaws[]    = { -180.0f, -90.0f, 0.0f, 37.0f, 179.9f };
    const float baseRolls[]   = { -45.0f, 0.0f, 45.0f };
    // Past what a neck reaches, because a tracker profile amplified for
    // shoulder checks gets there and nothing downstream clamps it.
    const float deltas[]      = { -120.0f, -25.0f, 0.0f, 25.0f, 120.0f };

    for (float bp : basePitches) {
        for (float by : baseYaws) {
            for (float br : baseRolls) {
                for (float d : deltas) {
                    float ang[3] = { bp, by, br };
                    ApplyCameraLocalRotation(ang, d, d, d);
                    const bool ok = std::isfinite(ang[0]) && std::isfinite(ang[1]) &&
                                    std::isfinite(ang[2]) && std::fabs(ang[0]) <= 90.001f &&
                                    std::fabs(ang[1]) <= 180.001f && std::fabs(ang[2]) <= 180.001f;
                    if (!ok) {
                        std::printf("    (p%.3f y%.1f r%.1f) + %.1f gave (p%.6g y%.6g r%.6g)\n",
                                    bp, by, br, d, ang[0], ang[1], ang[2]);
                        Check(false, "a finite pose always yields a renderable QAngle");
                        return;
                    }
                }
            }
        }
    }
    Check(true, "a finite pose always yields a renderable QAngle");
}

}  // namespace

int RunSourceMathTests() {
    std::printf("\nSource math\n===========\n");
    TestScaleFovByWidthRatio();
    TestAngleVectors();
    TestBasisToAngles();
    TestApplyCameraLocalRotation();
    TestAngleVectorsStayOrthonormal();
    TestCameraLocalCompositionStaysRenderable();
    return Check.failures();
}
