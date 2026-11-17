#include "ControllerState.h"

#include <algorithm>
#include <cmath>

const wchar_t* PadButtonName(PadButton b) {
    switch (b) {
        case PadButton::Cross: return L"Cross";
        case PadButton::Circle: return L"Circle";
        case PadButton::Square: return L"Square";
        case PadButton::Triangle: return L"Triangle";
        case PadButton::DpadUp: return L"D-pad up";
        case PadButton::DpadDown: return L"D-pad down";
        case PadButton::DpadLeft: return L"D-pad left";
        case PadButton::DpadRight: return L"D-pad right";
        case PadButton::L1: return L"L1";
        case PadButton::R1: return L"R1";
        case PadButton::L3: return L"L3";
        case PadButton::R3: return L"R3";
        case PadButton::Options: return L"Options";
        case PadButton::Create: return L"Create";
        case PadButton::Touchpad: return L"Touchpad";
        case PadButton::PS: return L"PS button";
        default: return L"?";
    }
}

std::int16_t StickAxis(float unit) {
    const float v = std::clamp(unit, -1.0f, 1.0f) * 32767.0f;
    return static_cast<std::int16_t>(std::lround(v));
}
