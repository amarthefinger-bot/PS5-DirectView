#pragma once

#include <cstdint>

// Digital buttons of a PlayStation controller, as DirectView models them.
// RemotePlayClient converts this to the Remote Play wire state.
enum class PadButton : std::uint8_t {
    Cross, Circle, Square, Triangle,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    L1, R1, L3, R3,
    Options, Create, Touchpad, PS,
    Count
};

constexpr int kPadButtonCount = static_cast<int>(PadButton::Count);

// One complete controller snapshot. Sticks use the DualSense/SDL convention:
// -32768..32767, +X = right, +Y = down. Triggers 0..255.
struct PadState {
    std::uint32_t buttons = 0;
    std::uint8_t l2 = 0, r2 = 0;
    std::int16_t lx = 0, ly = 0, rx = 0, ry = 0;

    bool Pressed(PadButton b) const { return (buttons >> static_cast<unsigned>(b)) & 1u; }
    void Set(PadButton b, bool down) {
        const std::uint32_t bit = 1u << static_cast<unsigned>(b);
        buttons = down ? (buttons | bit) : (buttons & ~bit);
    }
    bool IsNeutral() const { return *this == PadState{}; }
    bool operator==(const PadState&) const = default;
};

const wchar_t* PadButtonName(PadButton b);

// [-1, 1] -> stick axis value, rounded, saturating.
std::int16_t StickAxis(float unit);
