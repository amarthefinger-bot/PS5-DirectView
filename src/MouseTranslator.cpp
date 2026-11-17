#include "MouseTranslator.h"

#include <algorithm>
#include <cmath>

void MouseTranslator::Configure(const AimSettings& settings) {
    settings_ = settings;
    settings_.windowMs = std::clamp(settings_.windowMs, 2, 32);
}

void MouseTranslator::Reset() {
    head_ = count_ = 0;
    sumX_ = sumY_ = 0;
    lastMotion_ = -1e18;
    lastEval_ = -1.0;
    smoothed_ = {};
}

void MouseTranslator::AddMotion(double timeMs, int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    if (count_ == kCapacity) { // full: drop the oldest sample
        const Sample& old = ring_[head_];
        sumX_ -= old.dx;
        sumY_ -= old.dy;
        head_ = (head_ + 1) % kCapacity;
        --count_;
    }
    ring_[(head_ + count_) % kCapacity] = {timeMs, dx, dy};
    ++count_;
    sumX_ += dx;
    sumY_ += dy;
    lastMotion_ = timeMs;
}

void MouseTranslator::Expire(double cutoff) {
    while (count_ > 0 && ring_[head_].t <= cutoff) {
        sumX_ -= ring_[head_].dx;
        sumY_ -= ring_[head_].dy;
        head_ = (head_ + 1) % kCapacity;
        --count_;
    }
}

float MouseTranslator::Shape(float magnitude, const AimSettings& s) {
    float d = magnitude;
    if (d <= 0.0f) return 0.0f;
    d += (s.accelPct / 100.0f) * d * d;
    d = std::min(d, 1.0f);
    const float dz = s.deadzonePct / 100.0f;
    if (d <= dz) return 0.0f;
    d = (d - dz) / (1.0f - dz);
    if (s.curve == 1 && s.exponentPct != 100)
        d = std::pow(d, s.exponentPct / 100.0f);
    const float anti = s.antiDeadzonePct / 100.0f;
    d = anti + (1.0f - anti) * d;
    return std::clamp(d, 0.0f, 1.0f);
}

StickVector MouseTranslator::Evaluate(double timeMs) {
    const double dt = lastEval_ < 0.0 ? 0.0 : std::max(0.0, timeMs - lastEval_);
    lastEval_ = timeMs;
    const double window = settings_.windowMs;
    Expire(timeMs - window);

    // Recentre: no motion for recenterMs -> exactly neutral, no smoothing tail.
    if (timeMs - lastMotion_ > settings_.recenterMs || count_ == 0) {
        smoothed_ = {};
        return {};
    }

    const float vx = static_cast<float>(sumX_ / window);
    float vy = static_cast<float>(sumY_ / window) * (settings_.verticalPct / 100.0f);
    if (settings_.invertY) vy = -vy;
    const float gain = Gain(settings_.sensitivity);
    const float speed = std::sqrt(vx * vx + vy * vy);

    StickVector target{};
    if (speed > 0.0f) {
        const float d = Shape(speed * gain, settings_);
        target = {vx / speed * d, vy / speed * d};
    }

    if (settings_.smoothingMs > 0 && dt > 0.0) {
        const float a = static_cast<float>(1.0 - std::exp(-dt / settings_.smoothingMs));
        smoothed_.x += a * (target.x - smoothed_.x);
        smoothed_.y += a * (target.y - smoothed_.y);
    } else {
        smoothed_ = target;
    }

    // Circular stick gate: never report more than full deflection.
    StickVector out = smoothed_;
    const float len = std::sqrt(out.x * out.x + out.y * out.y);
    if (len > 1.0f) { out.x /= len; out.y /= len; }
    return out;
}
