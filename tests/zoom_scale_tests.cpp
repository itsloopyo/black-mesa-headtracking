// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
// Characterization tests for the zoom compensation the render-view detour
// applies to the head pose (src/zoom_scale.h, and the ratio fov_override.cpp
// builds from CViewSetup::fov).
//
// The faults these lock are all silent in a diff and silent in game. A factor
// that is wrong by a constant reads exactly like a factor that is right - the
// symptom is head tracking feeling weak everywhere rather than wrong anywhere -
// and the tangent round trip turns into a reflection past a quarter turn, which
// only shows on a pose a tracker profile amplified for shoulder checks reaches.

#include <cmath>
#include <cstdio>

#include "angles.h"
#include "source_math.h"
#include "zoom_scale.h"

namespace {

using headtracking::kDegToRad;
using headtracking::kMaxZoomScaledAngle;
using headtracking::kRadToDeg;
using headtracking::ScaleRotationForZoom;

int g_failures = 0;

void Check(bool cond, const char* name) {
    if (cond) {
        std::printf("  [PASS] %s\n", name);
    } else {
        std::printf("  [FAIL] %s\n", name);
        ++g_failures;
    }
}

bool NearEqual(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

// What the compensation is defined to be: the angle whose tangent is the
// original's, scaled. Written out here rather than called from the header, so
// a change to the header's arithmetic fails rather than agreeing with itself.
float ExpectedScaled(float deg, float factor) {
    return std::atan(std::tan(deg * kDegToRad) * factor) * kRadToDeg;
}

void TestUnzoomedFrameIsUntouched() {
    std::printf("ScaleRotationForZoom, a frame at the un-zoomed FOV\n");
    // Ordinary play. Not "close enough" - the pose has to be exactly what the
    // tracker sent, because this is every frame the player is not zooming.
    const float angles[] = { -120.0f, -89.0f, -30.0f, 0.0f, 0.5f, 30.0f, 89.0f, 120.0f };
    for (float deg : angles) {
        if (ScaleRotationForZoom(deg, 1.0f) != deg) {
            std::printf("    %.1f degrees changed at factor 1.0\n", deg);
            Check(false, "an un-zoomed frame leaves the angle bit-for-bit alone");
            return;
        }
    }
    Check(true, "an un-zoomed frame leaves the angle bit-for-bit alone");
}

void TestZoomShrinksTheAngle() {
    std::printf("ScaleRotationForZoom, a zoomed frame\n");
    // A 2x zoom halves the tangent, so the same head angle turns the camera
    // less and displaces the picture by the same distance it did un-zoomed.
    Check(NearEqual(ScaleRotationForZoom(10.0f, 0.5f), ExpectedScaled(10.0f, 0.5f)),
          "10 degrees at a 2x zoom is the tangent-scaled angle");
    Check(NearEqual(ScaleRotationForZoom(-10.0f, 0.5f), -ScaleRotationForZoom(10.0f, 0.5f)),
          "the scaling is odd, so left and right shrink alike");
    Check(ScaleRotationForZoom(10.0f, 0.5f) < 10.0f, "a zoom shrinks the head's contribution");
    Check(ScaleRotationForZoom(10.0f, 2.0f) > 10.0f,
          "a frame WIDER than the un-zoomed FOV grows it");
    // The mod's own [View] Fov is the wide case: the factor is above 1.0 there,
    // and the same head angle has to turn the camera further to cover the same
    // fraction of a wider frame.
    Check(NearEqual(ScaleRotationForZoom(15.0f, 1.0f / 0.5f), ExpectedScaled(15.0f, 2.0f)),
          "the wide case goes through the same tangent round trip");
}

void TestSaturationIsContinuous() {
    std::printf("ScaleRotationForZoom, past the quarter turn\n");
    // tan() has crossed its asymptote by 91 degrees and atan() answers in the
    // first quadrant, so an unguarded round trip there returns the angle on the
    // opposite side and the view flips half a turn between two frames.
    Check(ScaleRotationForZoom(91.0f, 0.5f) > 0.0f, "an angle past vertical keeps its sign");
    Check(ScaleRotationForZoom(-91.0f, 0.5f) < 0.0f, "and so does its mirror");

    // The remainder is added back rather than the angle being passed through
    // whole, so the two branches meet exactly at the limit.
    const float atLimit = ScaleRotationForZoom(kMaxZoomScaledAngle, 0.5f);
    const float justPast = ScaleRotationForZoom(kMaxZoomScaledAngle + 0.001f, 0.5f);
    Check(NearEqual(atLimit, justPast, 2e-3f), "no step at the saturation join");

    // Monotone across it: a head that keeps turning must keep turning the view
    // the same way.
    float previous = ScaleRotationForZoom(80.0f, 0.5f);
    for (float deg = 80.1f; deg <= 120.0f; deg += 0.1f) {
        const float current = ScaleRotationForZoom(deg, 0.5f);
        if (!(current > previous)) {
            std::printf("    %.1f degrees mapped to %.4f, back from %.4f\n", deg, current,
                        previous);
            Check(false, "monotone from 80 through 120 degrees");
            return;
        }
        previous = current;
    }
    Check(true, "monotone from 80 through 120 degrees");
}

void TestUnreadableFactorScalesNothing() {
    std::printf("ScaleRotationForZoom, a factor that is not a factor\n");
    // A mod that cannot read the live FOV applies no compensation rather than a
    // guessed one - fov_override.cpp returns 1.0 for that, and these are the
    // arithmetic's own guards behind it.
    Check(ScaleRotationForZoom(12.0f, 0.0f) == 12.0f, "a zero factor passes the angle through");
    Check(ScaleRotationForZoom(12.0f, -0.5f) == 12.0f,
          "a negative factor passes the angle through");
    const float nan = std::nanf("");
    Check(ScaleRotationForZoom(12.0f, nan) == 12.0f, "a NaN factor passes the angle through");
    Check(std::isnan(ScaleRotationForZoom(nan, 0.5f)), "a NaN angle is not invented into one");
}

// The units trap, in the form it takes in this game. CViewSetup::fov is
// HORIZONTAL degrees already widened for the viewport, and the base is the 4:3
// fov_desired put through the same widening - so both sides are the same axis
// and the widening cancels. Pairing a horizontal live value with a vertical
// base would not cancel: it would leave a constant multiplier on the pose
// through all of normal play, which is exactly the fault that has no symptom
// other than tracking feeling weak.
void TestFactorIsTheSameOnEitherAxis() {
    std::printf("the FOV ratio's units\n");
    using headtracking::source::kReferenceAspectInverse;
    using headtracking::source::ScaleFovByWidthRatio;

    const float aspect = 1920.0f / 1080.0f;
    const float ratio = aspect * kReferenceAspectInverse;
    const float baseHorizontal = ScaleFovByWidthRatio(90.0f, ratio);   // the un-zoomed frame
    const float liveHorizontal = ScaleFovByWidthRatio(50.0f, ratio);   // the suit zoom
    Check(NearEqual(baseHorizontal, 106.260201f), "fov_desired 90 widens to 106.26 at 16:9");

    const auto tanHalf = [](float deg) { return std::tan(deg * 0.5f * kDegToRad); };
    // tan(v/2) = tan(h/2) / aspect is the engine's own projection, so this is
    // the same two frames measured vertically.
    const float horizontalFactor = tanHalf(liveHorizontal) / tanHalf(baseHorizontal);
    const float verticalFactor =
        (tanHalf(liveHorizontal) / aspect) / (tanHalf(baseHorizontal) / aspect);
    Check(NearEqual(horizontalFactor, verticalFactor, 1e-5f),
          "the factor is the same measured horizontally or vertically");
    Check(NearEqual(horizontalFactor, tanHalf(50.0f) / tanHalf(90.0f), 1e-5f),
          "and the viewport widening cancels out of it");
    Check(horizontalFactor < 1.0f, "a 50 degree frame against a 90 degree base is a zoom");

    // The gate the log line exists to show: an un-zoomed frame is exactly 1.0.
    Check(NearEqual(tanHalf(baseHorizontal) / tanHalf(baseHorizontal), 1.0f, 1e-6f),
          "an un-zoomed frame is a factor of 1.0000");
}

}  // namespace

int RunZoomScaleTests() {
    std::printf("\nZoom compensation\n-----------------\n");
    TestUnzoomedFrameIsUntouched();
    TestZoomShrinksTheAngle();
    TestSaturationIsContinuous();
    TestUnreadableFactorScalesNothing();
    TestFactorIsTheSameOnEitherAxis();
    return g_failures;
}
