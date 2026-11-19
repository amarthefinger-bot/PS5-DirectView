#include "InputManager.h"

#ifdef _WIN32
#include <avrt.h>

#include <algorithm>
#include <cmath>

namespace {
constexpr wchar_t kWindowClass[] = L"PS5DirectViewInputSink";
constexpr double kWheelPulseMs = 50.0; // long enough to span several Remote Play sends
}

InputManager::~InputManager() { Stop(); }

double InputManager::NowMs() const {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * qpcToMs_;
}

void InputManager::Start(HWND mainWindow, PadSink sink) {
    if (thread_.joinable()) return;
    main_ = mainWindow;
    sink_ = std::move(sink);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcToMs_ = 1000.0 / static_cast<double>(f.QuadPart);
    {
        std::lock_guard lock(settingsMutex_);
        settings_ = pendingSettings_;
    }
    translator_.Configure(settings_.aim);
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { ThreadMain(); });
    WaitForSingleObject(ready_, 5000);
}

void InputManager::Stop() {
    if (!thread_.joinable()) return;
    if (window_) PostMessageW(window_, kMsgQuit, 0, 0);
    else if (threadId_) PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    thread_.join();
    if (ready_) { CloseHandle(ready_); ready_ = nullptr; }
    captured_ = false;
}

void InputManager::SetSettings(const InputSettings& settings) {
    {
        std::lock_guard lock(settingsMutex_);
        pendingSettings_ = settings;
    }
    if (window_) PostMessageW(window_, kMsgSettings, 0, 0);
}

void InputManager::SetCaptured(bool captured) {
    captured_ = captured;
    if (window_) PostMessageW(window_, kMsgCapture, captured ? 1 : 0, 0);
}

PadState InputManager::CurrentPad() const {
    std::lock_guard lock(statsMutex_);
    return current_;
}

InputStats InputManager::Stats() const {
    std::lock_guard lock(statsMutex_);
    return stats_;
}

// ---------------------------------------------------------------- thread

void InputManager::ThreadMain() {
    threadId_ = GetCurrentThreadId();
    // Same scheduling class as games: keeps the 1 ms tick punctual without
    // competing with the capture/render threads (which are separate threads).
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Games", &taskIndex);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &InputManager::WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    window_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              wc.hInstance, nullptr);
    if (window_) SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    statsWindowStart_ = GetTickCount64();
    SetEvent(ready_);

    bool running = window_ != nullptr;
    while (running) {
        const DWORD handles = active_ && timer_ ? 1 : 0;
        const DWORD r = MsgWaitForMultipleObjectsEx(handles, &timer_, INFINITE, QS_ALLINPUT,
                                                    MWMO_INPUTAVAILABLE);
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);
        if (handles && r == WAIT_OBJECT_0) {
            // 1 ms tick: decays the right stick, ends wheel pulses, watches focus.
            if (active_ && GetForegroundWindow() != main_) ReleaseAll(true, 0);
            if (active_) Evaluate(start, true);
            continue;
        }
        bool hadInput = false;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT || msg.message == kMsgQuit) { running = false; break; }
            if (msg.message == WM_INPUT) hadInput = true;
            DispatchMessageW(&msg);
        }
        // One evaluation per batch of raw input (8 kHz mice deliver bursts).
        if (running && hadInput && active_) Evaluate(start, false);
    }

    if (active_) ApplyCapture(false);
    if (timer_) { CloseHandle(timer_); timer_ = nullptr; }
    if (window_) { DestroyWindow(window_); window_ = nullptr; }
    UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

LRESULT CALLBACK InputManager::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<InputManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        switch (message) {
            case WM_INPUT:
                if (self->active_) self->OnRawInput(reinterpret_cast<HRAWINPUT>(lparam));
                break; // DefWindowProc performs raw-input cleanup
            case kMsgCapture:
                if ((wparam != 0) != self->active_) self->ApplyCapture(wparam != 0);
                return 0;
            case kMsgSettings: {
                std::lock_guard lock(self->settingsMutex_);
                self->settings_ = self->pendingSettings_;
                self->translator_.Configure(self->settings_.aim);
                return 0;
            }
            default: break;
        }
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void InputManager::ApplyCapture(bool captured) {
    RAWINPUTDEVICE devices[2]{};
    devices[0].usUsagePage = 0x01; devices[0].usUsage = 0x02; // mouse
    devices[1].usUsagePage = 0x01; devices[1].usUsage = 0x06; // keyboard
    if (captured) {
        for (auto& d : devices) {
            d.dwFlags = RIDEV_NOLEGACY | RIDEV_INPUTSINK;
            d.hwndTarget = window_;
        }
        registered_ = RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
        if (!registered_) { captured_ = false; PostMessageW(main_, kReleasedMessage, 0, 0); return; }
        std::fill(std::begin(down_), std::end(down_), false);
        wheelUntil_[0] = wheelUntil_[1] = 0;
        translator_.Reset();
        sentValid_ = false;
        active_ = true;
        LARGE_INTEGER due;
        due.QuadPart = -10'000; // 1 ms, then every 1 ms
        if (timer_) SetWaitableTimer(timer_, &due, 1, nullptr, nullptr, FALSE);
        lastTickMs_ = NowMs();
    } else {
        if (registered_) {
            for (auto& d : devices) { d.dwFlags = RIDEV_REMOVE; d.hwndTarget = nullptr; }
            RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
            registered_ = false;
        }
        if (timer_) CancelWaitableTimer(timer_);
        active_ = false;
        std::fill(std::begin(down_), std::end(down_), false);
        translator_.Reset();
        // Release every button and centre both sticks on the console.
        const PadState neutral{};
        if (sink_) sink_(neutral);
        sent_ = neutral;
        sentValid_ = true;
        std::lock_guard lock(statsMutex_);
        current_ = neutral;
    }
}

void InputManager::ReleaseAll(bool notifyMain, WPARAM reason) {
    captured_ = false;
    if (active_) ApplyCapture(false);
    if (notifyMain) PostMessageW(main_, kReleasedMessage, reason, 0);
}

void InputManager::SetCode(InputCode code, bool down, double nowMs) {
    if (code == 0 || code >= input_code::kMax) return;
    if (down && !down_[code]) pressTime_[code] = nowMs;
    down_[code] = down;
}

void InputManager::OnRawInput(HRAWINPUT handle) {
    RAWINPUT raw{};
    UINT size = sizeof(raw);
    if (GetRawInputData(handle, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1))
        return;
    const double now = NowMs();

    if (raw.header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& m = raw.data.mouse;
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY)) {
            translator_.AddMotion(now, m.lLastX, m.lLastY);
            std::lock_guard lock(statsMutex_);
            ++mouseEvents_;
        }
        const USHORT f = m.usButtonFlags;
        using namespace input_code;
        if (f & RI_MOUSE_LEFT_BUTTON_DOWN) SetCode(kMouseLeft, true, now);
        if (f & RI_MOUSE_LEFT_BUTTON_UP) SetCode(kMouseLeft, false, now);
        if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) SetCode(kMouseRight, true, now);
        if (f & RI_MOUSE_RIGHT_BUTTON_UP) SetCode(kMouseRight, false, now);
        if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) SetCode(kMouseMiddle, true, now);
        if (f & RI_MOUSE_MIDDLE_BUTTON_UP) SetCode(kMouseMiddle, false, now);
        if (f & RI_MOUSE_BUTTON_4_DOWN) SetCode(kMouseX1, true, now);
        if (f & RI_MOUSE_BUTTON_4_UP) SetCode(kMouseX1, false, now);
        if (f & RI_MOUSE_BUTTON_5_DOWN) SetCode(kMouseX2, true, now);
        if (f & RI_MOUSE_BUTTON_5_UP) SetCode(kMouseX2, false, now);
        if (f & RI_MOUSE_WHEEL) {
            const short delta = static_cast<short>(m.usButtonData);
            if (delta > 0) { wheelUntil_[0] = now + kWheelPulseMs; pressTime_[kWheelUp] = now; }
            if (delta < 0) { wheelUntil_[1] = now + kWheelPulseMs; pressTime_[kWheelDown] = now; }
        }
    } else if (raw.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& k = raw.data.keyboard;
        if (k.VKey == 0xFF || k.VKey == 0) return; // fake keys of escape sequences
        const bool up = (k.Flags & RI_KEY_BREAK) != 0;
        const bool e0 = (k.Flags & RI_KEY_E0) != 0;
        UINT vk = k.VKey;
        switch (vk) {
            case VK_SHIFT: vk = MapVirtualKeyW(k.MakeCode, MAPVK_VSC_TO_VK_EX); break;
            case VK_CONTROL: vk = e0 ? VK_RCONTROL : VK_LCONTROL; break;
            case VK_MENU: vk = e0 ? VK_RMENU : VK_LMENU; break;
            default: break;
        }
        // Release hotkeys: handled here, never forwarded to the console.
        if (vk == VK_F8 || vk == VK_ESCAPE) {
            if (!up) ReleaseAll(true, 1);
            return;
        }
        if (vk > 0 && vk < 0x100) SetCode(static_cast<InputCode>(vk), !up, now);
    }
}

PadState InputManager::BuildPad(double now) {
    using namespace input_code;
    auto codeActive = [&](InputCode c) {
        if (c == kNone) return false;
        if (c == kWheelUp) return now < wheelUntil_[0];
        if (c == kWheelDown) return now < wheelUntil_[1];
        return c < kMax && down_[c];
    };
    auto active = [&](Action a) {
        for (InputCode c : settings_.bindings[static_cast<size_t>(a)])
            if (codeActive(c)) return true;
        return false;
    };
    auto lastPress = [&](Action a) {
        double t = -1.0;
        for (InputCode c : settings_.bindings[static_cast<size_t>(a)])
            if (codeActive(c)) t = std::max(t, pressTime_[c]);
        return t;
    };

    PadState pad;
    for (int i = 0; i < kPadButtonCount; ++i)
        pad.Set(static_cast<PadButton>(i), active(static_cast<Action>(i)));
    pad.l2 = active(Action::L2) ? 255 : 0;
    pad.r2 = active(Action::R2) ? 255 : 0;

    // Left stick from keys; opposite keys: the most recently pressed wins.
    auto axis = [&](Action negative, Action positive) {
        const double n = lastPress(negative), p = lastPress(positive);
        if (n < 0 && p < 0) return 0.0f;
        return p > n ? 1.0f : -1.0f;
    };
    float lx = axis(Action::MoveLeft, Action::MoveRight);
    float ly = axis(Action::MoveUp, Action::MoveDown); // +y = down/back
    const float len = std::sqrt(lx * lx + ly * ly);
    if (len > 1.0f) { lx /= len; ly /= len; }
    if (active(Action::Walk)) {
        const float k = settings_.walkPct / 100.0f;
        lx *= k;
        ly *= k;
    }
    pad.lx = StickAxis(lx);
    pad.ly = StickAxis(ly);

    const StickVector right = translator_.Evaluate(now);
    pad.rx = StickAxis(right.x);
    pad.ry = StickAxis(right.y);
    return pad;
}

void InputManager::Evaluate(LARGE_INTEGER start, bool timed) {
    const double now = NowMs();
    const PadState pad = BuildPad(now);
    const bool changed = !sentValid_ || pad != sent_;
    if (changed && sink_) sink_(pad);
    if (changed) { sent_ = pad; sentValid_ = true; }
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);

    std::lock_guard lock(statsMutex_);
    current_ = pad;
    if (changed) ++padUpdates_;
    if (!timed) {
        const double us = static_cast<double>(end.QuadPart - start.QuadPart) * qpcToMs_ * 1000.0;
        procSum_ += us;
        procMax_ = std::max(procMax_, us);
        ++procSamples_;
    } else {
        const double late = now - lastTickMs_ - 1.0;
        if (late >= 0.0 && late < 100.0) { tickJitterSum_ += late * 1000.0; ++tickSamples_; }
        lastTickMs_ = now;
    }
    const ULONGLONG tick = GetTickCount64();
    const ULONGLONG elapsed = tick - statsWindowStart_;
    if (elapsed >= 1000) {
        const double secs = static_cast<double>(elapsed) / 1000.0;
        stats_.mouseHz = static_cast<double>(mouseEvents_) / secs;
        stats_.padUpdatesPerSec = static_cast<double>(padUpdates_) / secs;
        stats_.processAvgUs = procSamples_ ? procSum_ / static_cast<double>(procSamples_) : 0.0;
        stats_.processMaxUs = procMax_;
        stats_.tickJitterUs = tickSamples_ ? tickJitterSum_ / static_cast<double>(tickSamples_) : 0.0;
        mouseEvents_ = padUpdates_ = procSamples_ = tickSamples_ = 0;
        procSum_ = procMax_ = tickJitterSum_ = 0;
        statsWindowStart_ = tick;
    }
}

#endif // _WIN32
