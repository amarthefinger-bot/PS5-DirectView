#pragma once

#include "ControllerState.h"
#include "InputSettings.h"
#include "MouseTranslator.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
  #include <windows.h>
#endif

struct InputStats {
    double mouseHz = 0;
    double padUpdatesPerSec = 0;
    double processAvgUs = 0;
    double processMaxUs = 0;
    double tickJitterUs = 0;
};

#ifdef _WIN32

class InputManager {
public:
    using PadSink = std::function<void(const PadState&)>;
    static constexpr UINT kReleasedMessage = WM_APP + 40;

    InputManager() = default;
    ~InputManager();
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    void Start(HWND mainWindow, PadSink sink);
    void Stop();

    void SetSettings(const InputSettings& settings);
    void SetCaptured(bool captured);
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
    bool registered_ = false;
    bool active_ = false;

    mutable std::mutex settingsMutex_;
    InputSettings pendingSettings_;
    InputSettings settings_;

    MouseTranslator translator_;
    bool down_[input_code::kMax]{};
    double pressTime_[input_code::kMax]{};
    double wheelUntil_[2]{};
    PadState sent_;
    bool sentValid_ = false;
    double qpcToMs_ = 0;

    mutable std::mutex statsMutex_;
    PadState current_;
    InputStats stats_;
    std::uint64_t mouseEvents_ = 0, padUpdates_ = 0, procSamples_ = 0, tickSamples_ = 0;
    double procSum_ = 0, procMax_ = 0, tickJitterSum_ = 0;
    ULONGLONG statsWindowStart_ = 0;
    double lastTickMs_ = 0;
};

#else

// macOS InputManager: CGEventTap + IOHID based. The main window pointer is an
// NSWindow* bridged through void*. PadSink called off the CFRunLoop thread.
class InputManager {
public:
    using PadSink = std::function<void(const PadState&)>;
    // Mac posts kReleasedNotification via NSNotificationCenter instead of WM_APP.
    static constexpr int kReleasedByKey = 1;
    static constexpr int kReleasedByFocus = 0;
    using ReleaseSink = std::function<void(int reason)>;

    InputManager() = default;
    ~InputManager();
    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    void Start(void* nsWindow, PadSink sink, ReleaseSink onRelease = {});
    void Stop();

    void SetSettings(const InputSettings& settings);
    void SetCaptured(bool captured);
    bool Captured() const { return captured_.load(); }

    PadState CurrentPad() const;
    InputStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> captured_{false};
    mutable std::mutex statsMutex_;
    PadState current_;
    InputStats stats_;
    friend struct Impl;
};

#endif
