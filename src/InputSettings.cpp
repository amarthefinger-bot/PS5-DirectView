#include "InputSettings.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace {
constexpr wchar_t kKey[] = L"Software\\PS5DirectView\\Input";
constexpr DWORD kBindingsVersion = 1;

DWORD Load(const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &value,
                        &size) == ERROR_SUCCESS ? value : fallback;
}

int LoadInt(const wchar_t* name, int fallback) {
    return static_cast<int>(Load(name, static_cast<DWORD>(fallback)));
}

void Save(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &value, sizeof(value));
}

void Bind(Bindings& b, Action a, InputCode first, InputCode second = 0) {
    b[static_cast<size_t>(a)] = {first, second};
}
} // namespace

Bindings DefaultBindings() {
    using namespace input_code;
    Bindings b{};
    // Required defaults.
    Bind(b, Action::MoveUp, 'W');
    Bind(b, Action::MoveLeft, 'A');
    Bind(b, Action::MoveDown, 'S');
    Bind(b, Action::MoveRight, 'D');
    Bind(b, Action::L3, VK_LSHIFT);
    Bind(b, Action::Cross, VK_SPACE);
    Bind(b, Action::Square, 'R');
    Bind(b, Action::Triangle, 'E');
    Bind(b, Action::Circle, VK_LCONTROL);
    Bind(b, Action::L1, 'Q');
    Bind(b, Action::R2, kMouseLeft);
    Bind(b, Action::L2, kMouseRight);
    // Sensible extras (all rebindable).
    Bind(b, Action::R1, 'F');
    Bind(b, Action::R3, 'V', kMouseMiddle);
    Bind(b, Action::DpadUp, VK_UP, '1');
    Bind(b, Action::DpadRight, VK_RIGHT, '2');
    Bind(b, Action::DpadDown, VK_DOWN, '3');
    Bind(b, Action::DpadLeft, VK_LEFT, '4');
    Bind(b, Action::Options, VK_RETURN);
    Bind(b, Action::Create, VK_BACK);
    Bind(b, Action::Touchpad, VK_TAB);
    Bind(b, Action::PS, VK_HOME);
    Bind(b, Action::Walk, VK_LMENU);
    return b;
}

InputSettings::InputSettings() : bindings(DefaultBindings()) {}

void InputSettings::Clamp() {
    walkPct = std::clamp(walkPct, 10, 100);
    aim.sensitivity = std::clamp(aim.sensitivity, 1, 200);
    aim.verticalPct = std::clamp(aim.verticalPct, 25, 300);
    aim.deadzonePct = std::clamp(aim.deadzonePct, 0, 30);
    aim.antiDeadzonePct = std::clamp(aim.antiDeadzonePct, 0, 50);
    aim.curve = std::clamp(aim.curve, 0, 1);
    aim.exponentPct = std::clamp(aim.exponentPct, 50, 300);
    aim.accelPct = std::clamp(aim.accelPct, 0, 200);
    aim.smoothingMs = std::clamp(aim.smoothingMs, 0, 50);
    aim.windowMs = std::clamp(aim.windowMs, 2, 32);
    aim.recenterMs = std::clamp(aim.recenterMs, 2, 100);
    for (auto& slots : bindings)
        for (auto& code : slots)
            if (code >= input_code::kMax) code = input_code::kNone;
}

const wchar_t* ActionName(Action a) {
    const int i = static_cast<int>(a);
    if (i < kPadButtonCount) return PadButtonName(static_cast<PadButton>(i));
    switch (a) {
        case Action::L2: return L"L2";
        case Action::R2: return L"R2";
        case Action::MoveUp: return L"Move forward";
        case Action::MoveDown: return L"Move back";
        case Action::MoveLeft: return L"Move left";
        case Action::MoveRight: return L"Move right";
        case Action::Walk: return L"Walk (slow stick)";
        default: return L"?";
    }
}

std::wstring InputCodeName(InputCode code) {
    using namespace input_code;
    switch (code) {
        case kNone: return L"\x2014";
        case kMouseLeft: return L"Mouse left";
        case kMouseRight: return L"Mouse right";
        case kMouseMiddle: return L"Mouse middle";
        case kMouseX1: return L"Mouse 4";
        case kMouseX2: return L"Mouse 5";
        case kWheelUp: return L"Wheel up";
        case kWheelDown: return L"Wheel down";
        default: break;
    }
    if (code > 0xFF) return L"?";
    switch (code) {
        case VK_LSHIFT: return L"Left Shift";
        case VK_RSHIFT: return L"Right Shift";
        case VK_LCONTROL: return L"Left Ctrl";
        case VK_RCONTROL: return L"Right Ctrl";
        case VK_LMENU: return L"Left Alt";
        case VK_RMENU: return L"Right Alt";
        case VK_UP: return L"Up";
        case VK_DOWN: return L"Down";
        case VK_LEFT: return L"Left";
        case VK_RIGHT: return L"Right";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"Page Up";
        case VK_NEXT: return L"Page Down";
        case VK_INSERT: return L"Insert";
        case VK_DELETE: return L"Delete";
        default: break;
    }
    UINT scan = MapVirtualKeyW(code, MAPVK_VK_TO_VSC_EX);
    LONG lparam = static_cast<LONG>((scan & 0xFF) << 16);
    if (scan & 0xFF00) lparam |= 1 << 24; // extended key
    wchar_t name[64]{};
    if (scan && GetKeyNameTextW(lparam, name, 64) > 0) return name;
    wchar_t fallback[16];
    swprintf_s(fallback, L"Key 0x%02X", code);
    return fallback;
}

InputSettings LoadInputSettings() {
    InputSettings s;
    s.enabled = Load(L"Enabled", 1) != 0;
    s.autoReconnect = Load(L"AutoReconnect", 1) != 0;
    s.connectOnStartup = Load(L"ConnectOnStartup", 0) != 0;
    s.walkPct = LoadInt(L"WalkPct", s.walkPct);
    AimSettings& a = s.aim;
    a.sensitivity = LoadInt(L"Sensitivity", a.sensitivity);
    a.verticalPct = LoadInt(L"VerticalPct", a.verticalPct);
    a.deadzonePct = LoadInt(L"DeadzonePct", a.deadzonePct);
    a.antiDeadzonePct = LoadInt(L"AntiDeadzonePct", a.antiDeadzonePct);
    a.curve = LoadInt(L"Curve", a.curve);
    a.exponentPct = LoadInt(L"ExponentPct", a.exponentPct);
    a.accelPct = LoadInt(L"AccelPct", a.accelPct);
    a.smoothingMs = LoadInt(L"SmoothingMs", a.smoothingMs);
    a.windowMs = LoadInt(L"WindowMs", a.windowMs);
    a.recenterMs = LoadInt(L"RecenterMs", a.recenterMs);
    a.invertY = Load(L"InvertY", 0) != 0;

    // Bindings: version DWORD + kActionCount * kSlotsPerAction uint16 codes.
    std::uint8_t blob[4 + sizeof(Bindings)]{};
    DWORD size = sizeof(blob);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, L"Bindings", RRF_RT_REG_BINARY, nullptr, blob,
                     &size) == ERROR_SUCCESS && size == sizeof(blob)) {
        DWORD version = 0;
        std::memcpy(&version, blob, 4);
        if (version == kBindingsVersion) std::memcpy(&s.bindings, blob + 4, sizeof(Bindings));
    }
    s.Clamp();
    return s;
}

void SaveInputSettings(const InputSettings& s) {
    Save(L"Enabled", s.enabled);
    Save(L"AutoReconnect", s.autoReconnect);
    Save(L"ConnectOnStartup", s.connectOnStartup);
    Save(L"WalkPct", static_cast<DWORD>(s.walkPct));
    const AimSettings& a = s.aim;
    Save(L"Sensitivity", static_cast<DWORD>(a.sensitivity));
    Save(L"VerticalPct", static_cast<DWORD>(a.verticalPct));
    Save(L"DeadzonePct", static_cast<DWORD>(a.deadzonePct));
    Save(L"AntiDeadzonePct", static_cast<DWORD>(a.antiDeadzonePct));
    Save(L"Curve", static_cast<DWORD>(a.curve));
    Save(L"ExponentPct", static_cast<DWORD>(a.exponentPct));
    Save(L"AccelPct", static_cast<DWORD>(a.accelPct));
    Save(L"SmoothingMs", static_cast<DWORD>(a.smoothingMs));
    Save(L"WindowMs", static_cast<DWORD>(a.windowMs));
    Save(L"RecenterMs", static_cast<DWORD>(a.recenterMs));
    Save(L"InvertY", a.invertY);
    std::uint8_t blob[4 + sizeof(Bindings)]{};
    std::memcpy(blob, &kBindingsVersion, 4);
    std::memcpy(blob + 4, &s.bindings, sizeof(Bindings));
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, L"Bindings", REG_BINARY, blob, sizeof(blob));
}
