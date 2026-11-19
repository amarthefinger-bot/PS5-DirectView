#pragma once
#include <cmath>
#include <cstdint>

// Explicit 30 fps content inside a 59.94/60 Hz capture stream. This is not
// automatic duplicate detection: animated 60 fps HUDs are sampled at 30 fps.
class FrameGenerationCadence {
public:
    enum class Step { Passthrough, Seed, Endpoint, Midpoint };
    void Reset() { sequence_ = 0; seeded_ = false; endpoint_ = false; }
    Step Next(bool enabled, double captureFps, std::uint64_t sequence) {
        if (!enabled || !std::isfinite(captureFps) || captureFps < 59.0 || captureFps > 61.0) {
            Reset(); return Step::Passthrough;
        }
        if (seeded_ && sequence != sequence_ + 1) Reset();
        sequence_ = sequence;
        if (!seeded_) { seeded_ = true; endpoint_ = true; return Step::Seed; }
        if (endpoint_) { endpoint_ = false; return Step::Endpoint; }
        endpoint_ = true;
        return Step::Midpoint;
    }
private:
    std::uint64_t sequence_ = 0;
    bool seeded_ = false, endpoint_ = false;
};
