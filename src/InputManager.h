#pragma once

#include "ControllerState.h"
#include "InputSettings.h"
#include "MouseTranslator.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

struct InputStats {
    double mouseHz = 0;            // raw mouse reports per second while captured
    double padUpdatesPerSec = 0;   // controller states handed to Remote Play
    double processAvgUs = 0;       // WM_INPUT received -> controller state submitted
    double processMaxUs = 0;
    double tickJitterUs = 0;       // input-thread timer lateness (avg)
};

// Mouse & keyboard -> controller emulation on a dedicated thread.
//
// While captured, raw input (WM_INPUT) for mouse and keyboard is registered
// with RIDEV_NOLEGACY on a message-only window owned by this thread, so:
//   * the main/render thread receives no mouse/keyboard messages at all,
//   * keys don't reach DirectView shortcuts or Windows (system keys such as
//     Alt+Tab / Win / Ctrl+Alt+Del still work),
// and the cursor is clipped by the main thread so the desktop doesn't move.
// F8 or Esc releases capture (handled here, never sent to the PS5).
class InputManager {
public:
    using PadSink = std::function<void(const PadState&)>;
    // Posted to the main window when the input thread released capture
    // itself (F8/Esc pressed, focus lost). wParam: 1 = by key, 0 = focus.
    static constexpr UINT kReleasedMessage = WM_APP + 40;

    InputManager() = default;
    ~InputManager();
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    void Start(HWND mainWindow, PadSink sink);
    void Stop();

    void SetSettings(const InputSettings& settings); // thread-safe copy
    void SetCaptured(bool captured);                  // main thread
    bool Captured() const { return captured_.load(); }

    PadState CurrentPad() const;
    InputStats Stats() const;

private:
    enum : UINT { kMsgCapture = WM_APP + 1, kMsgSettings = WM_APP + 2, kMsgQuit = WM_APP + 3 };

    void ThreadMain();
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void OnRawInput(HRAWINPUT handle);
    void ApplyCapture(bool captured);
    void ReleaseAll(bool notifyMain, WPARAM reason);
    void Evaluate(LARGE_INTEGER start, bool timed);
    PadState BuildPad(double nowMs);
    void SetCode(InputCode code, bool down, double nowMs);
    double NowMs() const;

    HWND main_ = nullptr;
    HWND window_ = nullptr;
    DWORD threadId_ = 0;
    std::thread thread_;
    HANDLE ready_ = nullptr;
    HANDLE timer_ = nullptr;
    PadSink sink_;
    std::atomic<bool> captured_{false};
    bool registered_ = false; // input thread only
    bool active_ = false;     // input thread: capture applied

    mutable std::mutex settingsMutex_;
    InputSettings pendingSettings_;
    InputSettings settings_;  // input thread copy

    // Input-thread state.
    MouseTranslator translator_;
    bool down_[input_code::kMax]{};
    double pressTime_[input_code::kMax]{};
    double wheelUntil_[2]{};  // wheel up/down pulse end
    PadState sent_;
    bool sentValid_ = false;
    double qpcToMs_ = 0;

    // Stats (written by input thread, read by UI).
    mutable std::mutex statsMutex_;
    PadState current_;
    InputStats stats_;
    std::uint64_t mouseEvents_ = 0, padUpdates_ = 0, procSamples_ = 0, tickSamples_ = 0;
    double procSum_ = 0, procMax_ = 0, tickJitterSum_ = 0;
    ULONGLONG statsWindowStart_ = 0;
    double lastTickMs_ = 0;
};
