// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo / CameraUnlock
#pragma once

#include <cmath>
#include <cstdio>

namespace headtracking::tests {

// The whole harness. Every suite declares one of these in its own anonymous
// namespace, calls it once per assertion, and returns its failure count to
// test_main.cpp - which sums the suites, so each count has to stay that suite's
// own rather than a running total.
class CheckCounter {
public:
    void operator()(bool passed, const char* name) {
        if (passed) {
            std::printf("  [PASS] %s\n", name);
            return;
        }
        std::printf("  [FAIL] %s\n", name);
        ++m_failures;
    }

    int failures() const { return m_failures; }

private:
    int m_failures = 0;
};

// Float comparison for the geometry suites. The default tolerance is the one
// the camera maths was characterized at; a suite that needs a tighter one
// passes it per call.
inline bool NearEqual(float a, float b, float eps = 1e-3f) {
    return std::fabs(a - b) <= eps;
}

}  // namespace headtracking::tests
