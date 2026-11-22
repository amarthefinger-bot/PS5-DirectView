#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>

// Presentation pacing for a 30-fps *capture* stream. Unlike frame generation,
// this does not synthesize any pixels or touch the input/Remote Play pipeline.
// The caller owns a single pending video frame and presents the newest one at
// the next deadline. Reset the clock on mode changes and lengthy stalls.
class FramePacer {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;

    void Reset() { armed_ = false; period_ = Duration::zero(); next_ = {}; }
    bool Armed() const { return armed_; }

    void Start(double fps, TimePoint now) {
        if (!std::isfinite(fps) || fps < 1.0) fps = 30.0;
        period_ = std::chrono::duration_cast<Duration>(std::chrono::duration<double>(1.0 / fps));
        if (period_ <= Duration::zero()) period_ = std::chrono::milliseconds(33);
        next_ = now; // do not add latency to the first captured frame
        armed_ = true;
    }

    bool Due(TimePoint now) const { return armed_ && now >= next_; }
    Duration Remaining(TimePoint now) const {
        if (!armed_ || now >= next_) return Duration::zero();
        return next_ - now;
    }

    // After drawing, advance on a regular grid. Skipping missed presentation
    // slots instead of bursting delayed frames prevents a long stall from
    // creating a visible rapid catch-up sequence.
    void Presented(TimePoint now) {
        if (!armed_) return;
        if (now > next_ + 2 * period_) {
            next_ = now + period_;
            return;
        }
        do { next_ += period_; } while (next_ <= now);
    }

    // A long gap in the capture stream (PS5 output change, device pause, etc.)
    // should not leave the next frame waiting on an obsolete clock.
    bool IsStalled(TimePoint now) const {
        return armed_ && now > next_ + 3 * period_;
    }

private:
    bool armed_ = false;
    Duration period_{};
    TimePoint next_{};
};
