#pragma once

#include "InputSettings.h"

#include <array>

struct StickVector {
    float x = 0.0f, y = 0.0f; // [-1, 1], +y = down
};

// Converts relative mouse counts into a right-stick deflection.
//
// A stick is a *velocity* control (deflection = turn speed) while a mouse is a
// *position* device, so the translator measures mouse speed over a short
// sliding window and maps speed -> deflection:
//
//   v      = counts in the last windowMs / windowMs          (counts per ms)
//   d      = |v| * gain(sensitivity)  (vertical axis scaled by verticalPct)
//   d     += accel * d^2                                      (acceleration)
//   d      = clamp(d, 0, 1)                                   (stick limit)
//   d      = 0 if d < deadzone, else rescaled to [0, 1]
//   d      = d ^ exponent                                     (power curve)
//   d      = anti + (1 - anti) * d   for d > 0                (anti-deadzone)
//   output = direction(v) * d, optionally smoothed, circularly clamped
//
// The stick re-centres recenterMs after the last mouse movement.
// Single-threaded: owned and driven by the input thread.
class MouseTranslator {
public:
    void Configure(const AimSettings& settings);
    void AddMotion(double timeMs, int dx, int dy);
    StickVector Evaluate(double timeMs);
    void Reset();

    // Gain such that |v| counts/ms * gain = deflection before curve/limits.
    static float Gain(int sensitivity) { return sensitivity * 0.004f; }
    // Speed -> deflection magnitude (no smoothing/direction). Exposed for tests/UI.
    static float Shape(float magnitude, const AimSettings& s);

private:
    struct Sample { double t; int dx, dy; };
    void Expire(double cutoff);

    AimSettings settings_{};
    static constexpr size_t kCapacity = 2048; // 8 kHz mouse * 32 ms window = 256
    std::array<Sample, kCapacity> ring_{};
    size_t head_ = 0, count_ = 0;
    long long sumX_ = 0, sumY_ = 0;
    double lastMotion_ = -1e18;
    double lastEval_ = -1.0;
    StickVector smoothed_{};
};
