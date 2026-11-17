#pragma once

#include "ControllerState.h"

#include <array>
#include <cstdint>
#include <string>

// Everything a binding can drive. The first kPadButtonCount entries map 1:1
// onto PadButton.
enum class Action : std::uint8_t {
    // Digital buttons (same order as PadButton).
    Cross, Circle, Square, Triangle,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    L1, R1, L3, R3,
    Options, Create, Touchpad, PS,
    // Analog triggers (fully pressed while held).
    L2, R2,
    // Left stick from keys.
    MoveUp, MoveDown, MoveLeft, MoveRight,
    Walk, // scales the left stick while held
    Count
};

constexpr int kActionCount = static_cast<int>(Action::Count);
constexpr int kSlotsPerAction = 2;

// Input codes: 1..255 = Windows virtual-key (left/right modifiers kept apart,
// e.g. VK_LSHIFT), 0x101.. = mouse. 0 = unbound.
using InputCode = std::uint16_t;
namespace input_code {
constexpr InputCode kNone = 0;
constexpr InputCode kMouseLeft = 0x101;
constexpr InputCode kMouseRight = 0x102;
constexpr InputCode kMouseMiddle = 0x103;
constexpr InputCode kMouseX1 = 0x104;
constexpr InputCode kMouseX2 = 0x105;
constexpr InputCode kWheelUp = 0x106;
constexpr InputCode kWheelDown = 0x107;
constexpr InputCode kMax = 0x108;      // exclusive upper bound
}

using Bindings = std::array<std::array<InputCode, kSlotsPerAction>, kActionCount>;

// Mouse -> right stick. See MouseTranslator.cpp for the exact maths.
struct AimSettings {
    int sensitivity = 50;     // 1..200   stick deflection per mouse speed
    int verticalPct = 100;    // 25..300  vertical sensitivity, % of horizontal
    int deadzonePct = 0;      // 0..30    ignore deflection below this (noise filter)
    int antiDeadzonePct = 12; // 0..50    jump past the game's own stick deadzone
    int curve = 0;            // 0 linear, 1 power curve
    int exponentPct = 100;    // 50..300  power-curve exponent x100 (100 = linear)
    int accelPct = 0;         // 0..200   extra gain for fast movements
    int smoothingMs = 0;      // 0..50    output smoothing time constant (0 = none)
    int windowMs = 8;         // 2..32    mouse-speed measurement window
    int recenterMs = 10;      // 2..100   centre the stick after this long without motion
    bool invertY = false;
};

struct InputSettings {
    bool enabled = true;          // mouse & keyboard controller emulation
    bool autoReconnect = true;
    bool connectOnStartup = false;
    int walkPct = 50;             // 10..100 left-stick scale while Walk is held
    AimSettings aim;
    Bindings bindings{};

    InputSettings();              // default bindings
    void Clamp();
};

Bindings DefaultBindings();
const wchar_t* ActionName(Action a);
std::wstring InputCodeName(InputCode code);

InputSettings LoadInputSettings();
void SaveInputSettings(const InputSettings& settings);
