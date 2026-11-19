#include "InputSettings.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <fstream>
  #include <filesystem>
  #include <pwd.h>
  #include <unistd.h>
  #include <sys/types.h>
#endif

namespace {

#ifdef _WIN32
constexpr wchar_t kKey[] = L"Software\\PS5DirectView\\Input";
#endif
constexpr std::uint32_t kBindingsVersion = 1;

#ifdef _WIN32
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
#else
// macOS: binary file at ~/Library/Application Support/PS5 DirectView/input.bin
// Layout: u32 version, followed by fixed-struct integers then Bindings blob.
struct MacInputFile {
    std::uint32_t version = 0;
    std::uint32_t enabled = 1;
    std::uint32_t autoReconnect = 1;
    std::uint32_t connectOnStartup = 0;
    std::int32_t walkPct = 50;
    AimSettings aim;
    std::uint32_t invertY = 0;
    Bindings bindings{};
};

std::filesystem::path InputPath() {
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    std::filesystem::path base = std::filesystem::path(home) /
        "Library" / "Application Support" / "PS5 DirectView";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base / "input.bin";
}
#endif

void Bind(Bindings& b, Action a, InputCode first, InputCode second = 0) {
    b[static_cast<size_t>(a)] = {first, second};
}

#ifndef _WIN32
// macOS virtual-key mapping. Carbon kVK_* values from <HIToolbox/Events.h>
// range [0x00, 0x80). We overlay them into the same InputCode space Windows
// uses (VK codes also live in 1..255). This is opaque — just stable IDs.
namespace mac_vk {
constexpr InputCode W = 0x0D, A = 0x00, S = 0x01, D = 0x02;
constexpr InputCode Q = 0x0C, E = 0x0E, R = 0x0F, F = 0x03, V = 0x09;
constexpr InputCode Digit1 = 0x12, Digit2 = 0x13, Digit3 = 0x14, Digit4 = 0x15;
constexpr InputCode LShift = 0x38;    // actually "shift" (no left/right distinction in older SDKs)
constexpr InputCode RShift = 0x3C;
constexpr InputCode LControl = 0x3B;
constexpr InputCode RControl = 0x3E;
constexpr InputCode LOption = 0x3A;   // Alt
constexpr InputCode ROption = 0x3D;
constexpr InputCode Space = 0x31;
constexpr InputCode Return = 0x24;
constexpr InputCode Delete = 0x33;    // Backspace on Mac keyboards
constexpr InputCode Tab = 0x30;
constexpr InputCode Escape = 0x35;
constexpr InputCode Home = 0x73;
constexpr InputCode End = 0x77;
constexpr InputCode PageUp = 0x74, PageDown = 0x79;
constexpr InputCode ForwardDelete = 0x75;
constexpr InputCode Left = 0x7B, Right = 0x7C, Down = 0x7D, Up = 0x7E;
}
#endif
} // namespace

Bindings DefaultBindings() {
    using namespace input_code;
    Bindings b{};
#ifdef _WIN32
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
#else
    using namespace mac_vk;
    Bind(b, Action::MoveUp, W);
    Bind(b, Action::MoveLeft, A);
    Bind(b, Action::MoveDown, S);
    Bind(b, Action::MoveRight, D);
    Bind(b, Action::L3, LShift);
    Bind(b, Action::Cross, Space);
    Bind(b, Action::Square, R);
    Bind(b, Action::Triangle, E);
    Bind(b, Action::Circle, LControl);
    Bind(b, Action::L1, Q);
    Bind(b, Action::R2, kMouseLeft);
    Bind(b, Action::L2, kMouseRight);
    Bind(b, Action::R1, F);
    Bind(b, Action::R3, V, kMouseMiddle);
    Bind(b, Action::DpadUp, Up, Digit1);
    Bind(b, Action::DpadRight, Right, Digit2);
    Bind(b, Action::DpadDown, Down, Digit3);
    Bind(b, Action::DpadLeft, Left, Digit4);
    Bind(b, Action::Options, Return);
    Bind(b, Action::Create, Delete);
    Bind(b, Action::Touchpad, Tab);
    Bind(b, Action::PS, Home);
    Bind(b, Action::Walk, LOption);
#endif
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
#ifdef _WIN32
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
    if (scan & 0xFF00) lparam |= 1 << 24;
    wchar_t name[64]{};
    if (scan && GetKeyNameTextW(lparam, name, 64) > 0) return name;
    wchar_t fallback[16];
    swprintf_s(fallback, L"Key 0x%02X", code);
    return fallback;
#else
    // Mac: Carbon kVK_* values. Table is small, hand-maintained.
    switch (code) {
        case 0x00: return L"A"; case 0x01: return L"S"; case 0x02: return L"D";
        case 0x03: return L"F"; case 0x04: return L"H"; case 0x05: return L"G";
        case 0x06: return L"Z"; case 0x07: return L"X"; case 0x08: return L"C";
        case 0x09: return L"V"; case 0x0B: return L"B"; case 0x0C: return L"Q";
        case 0x0D: return L"W"; case 0x0E: return L"E"; case 0x0F: return L"R";
        case 0x10: return L"Y"; case 0x11: return L"T";
        case 0x12: return L"1"; case 0x13: return L"2"; case 0x14: return L"3";
        case 0x15: return L"4"; case 0x16: return L"6"; case 0x17: return L"5";
        case 0x19: return L"9"; case 0x1A: return L"7"; case 0x1C: return L"8";
        case 0x1D: return L"0";
        case 0x1F: return L"O"; case 0x20: return L"U"; case 0x22: return L"I";
        case 0x23: return L"P"; case 0x25: return L"L"; case 0x26: return L"J";
        case 0x28: return L"K"; case 0x2D: return L"N"; case 0x2E: return L"M";
        case 0x24: return L"Return"; case 0x30: return L"Tab";
        case 0x31: return L"Space"; case 0x33: return L"Delete"; case 0x35: return L"Escape";
        case 0x38: return L"Shift"; case 0x3C: return L"Right Shift";
        case 0x3B: return L"Control"; case 0x3E: return L"Right Control";
        case 0x3A: return L"Option"; case 0x3D: return L"Right Option";
        case 0x37: return L"Command";
        case 0x73: return L"Home"; case 0x74: return L"Page Up";
        case 0x77: return L"End";  case 0x79: return L"Page Down";
        case 0x75: return L"Forward Delete";
        case 0x7B: return L"Left"; case 0x7C: return L"Right";
        case 0x7D: return L"Down"; case 0x7E: return L"Up";
        default: break;
    }
    static thread_local wchar_t fallback[16];
    std::swprintf(fallback, 16, L"Key 0x%02X", code);
    return fallback;
#endif
}

#ifdef _WIN32

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

#else

InputSettings LoadInputSettings() {
    InputSettings s;
    std::ifstream in(InputPath(), std::ios::binary);
    if (!in) { s.Clamp(); return s; }
    MacInputFile f;
    in.read(reinterpret_cast<char*>(&f), sizeof(f));
    if (in.gcount() != sizeof(f) || f.version != kBindingsVersion) { s.Clamp(); return s; }
    s.enabled = f.enabled != 0;
    s.autoReconnect = f.autoReconnect != 0;
    s.connectOnStartup = f.connectOnStartup != 0;
    s.walkPct = f.walkPct;
    s.aim = f.aim;
    s.aim.invertY = f.invertY != 0;
    s.bindings = f.bindings;
    s.Clamp();
    return s;
}

void SaveInputSettings(const InputSettings& s) {
    MacInputFile f{};
    f.version = kBindingsVersion;
    f.enabled = s.enabled ? 1 : 0;
    f.autoReconnect = s.autoReconnect ? 1 : 0;
    f.connectOnStartup = s.connectOnStartup ? 1 : 0;
    f.walkPct = s.walkPct;
    f.aim = s.aim;
    f.invertY = s.aim.invertY ? 1 : 0;
    f.bindings = s.bindings;
    std::ofstream out(InputPath(), std::ios::binary | std::ios::trunc);
    if (!out) return;
    out.write(reinterpret_cast<const char*>(&f), sizeof(f));
}

#endif
