#include "Audio.h"
#include "Capture.h"
#include "FramePacer.h"
#include "InputManager.h"
#include "RemotePlayClient.h"
#include "Renderer.h"
#include "Ui.h"

#ifdef _WIN32

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfidl.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr UINT_PTR kStatusTimer = 1;
constexpr float kSharpLevels[] = {0.0f, 0.3f, 0.6f, 1.0f};
constexpr wchar_t kRegistryKey[] = L"Software\\PS5DirectView";

std::wstring NarrowToWide(const std::string& text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                   static_cast<int>(text.size()), nullptr, 0);
    if (count == 0) return std::wstring(text.begin(), text.end());
    std::wstring output(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), output.data(), count);
    return output;
}

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

bool Contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

// Higher = more likely to be the PS5 HDMI capture card. USB capture cards
// (MS2109/MS2130 "USB Video", Elgato, AVerMedia, UGREEN...) win; built-in
// webcams, IR cameras and virtual cameras are pushed to the bottom.
int CaptureDeviceScore(const VideoDevice& device) {
    const std::wstring name = Lower(device.name);
    const std::wstring link = Lower(device.symbolicLink);
    int score = 0;
    static const wchar_t* kCapture[] = {
        L"usb video", L"capture", L"hdmi", L"cam link", L"elgato", L"avermedia",
        L"live gamer", L"ugreen", L"hagibis", L"macrosilicon", L"ms2109", L"ms2130",
        L"game", L"video grabber", L"usb3", L"uvc", L"4k", L"razer ripsaw",
    };
    static const wchar_t* kNotCapture[] = {
        L"integrated", L"webcam", L"web cam", L"facetime", L"ir camera", L"infrared",
        L"virtual", L"obs", L"droidcam", L"snap camera", L"broadcast", L"iriun",
        L"front", L"rear", L"hd camera", L"hd user facing", L"windows hello",
    };
    for (const auto* word : kCapture) if (Contains(name, word)) score += 100;
    for (const auto* word : kNotCapture) if (Contains(name, word)) score -= 200;
    if (link.starts_with(L"\\\\?\\usb#")) score += 20; // physically on USB
    return score;
}

// ---------------------------------------------------------------- registry

DWORD LoadDword(const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, name, RRF_RT_REG_DWORD,
                        nullptr, &value, &size) == ERROR_SUCCESS ? value : fallback;
}

void SaveDword(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegistryKey, name, REG_DWORD, &value, sizeof(value));
}

std::wstring ReadRegString(const wchar_t* name) {
    wchar_t buffer[2048]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, name, RRF_RT_REG_SZ,
                     nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

void WriteRegString(const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegistryKey, name, REG_SZ, value.c_str(),
                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

Settings LoadSettings() {
    Settings s;
    s.capture.resolution = static_cast<int>(std::min<DWORD>(LoadDword(L"CaptureResolution", 0), 3));
    s.capture.fps = static_cast<int>(std::min<DWORD>(LoadDword(L"CaptureFps", 0), 3));
    s.capture.output = static_cast<int>(std::min<DWORD>(LoadDword(L"CapturePixel", 0), 2));
    s.sharpLevel = static_cast<int>(std::min<DWORD>(LoadDword(L"Sharpening", 2), 3));
    s.rangeMode = static_cast<int>(std::min<DWORD>(LoadDword(L"Range", 0), 2));
    s.vsync = LoadDword(L"VSync", 0) != 0;
    s.smooth4k30 = LoadDword(L"Smooth4K30", 0) != 0;
    s.audioEnabled = LoadDword(L"Audio", 1) != 0;
    s.audioOutput = ReadRegString(L"AudioOutput");
    s.volume = static_cast<int>(std::min<DWORD>(LoadDword(L"Volume", 100), 100));
    s.muted = LoadDword(L"Muted", 0) != 0;
    s.hud = LoadDword(L"Hud", 0) != 0;
    s.crosshair = LoadDword(L"Crosshair", 0) != 0;
    s.crossStyle = static_cast<int>(std::min<DWORD>(LoadDword(L"CrossStyle", 0), 3));
    s.crossColor = static_cast<int>(std::min<DWORD>(LoadDword(L"CrossColor", 0), 5));
    s.crossSize = static_cast<int>(std::clamp<DWORD>(LoadDword(L"CrossSize", 14), 4, 48));
    s.overlayImage = ReadRegString(L"OverlayImage");
    s.overlayPos = static_cast<int>(std::min<DWORD>(LoadDword(L"OverlayPos", 0), 4));
    s.overlayScale = static_cast<int>(std::clamp<DWORD>(LoadDword(L"OverlayScale", 25), 5, 100));
    s.overlayOpacity = static_cast<int>(std::clamp<DWORD>(LoadDword(L"OverlayOpacity", 100), 5, 100));
    s.alwaysOnTop = LoadDword(L"AlwaysOnTop", 0) != 0;
    s.input = LoadInputSettings();
    return s;
}

void SaveSettings(const Settings& s) {
    SaveDword(L"CaptureResolution", s.capture.resolution);
    SaveDword(L"CaptureFps", s.capture.fps);
    SaveDword(L"CapturePixel", s.capture.output);
    SaveDword(L"Sharpening", s.sharpLevel);
    SaveDword(L"Range", s.rangeMode);
    SaveDword(L"VSync", s.vsync);
    SaveDword(L"Smooth4K30", s.smooth4k30);
    SaveDword(L"Audio", s.audioEnabled);
    WriteRegString(L"AudioOutput", s.audioOutput);
    SaveDword(L"Volume", s.volume);
    SaveDword(L"Muted", s.muted);
    SaveDword(L"Hud", s.hud);
    SaveDword(L"Crosshair", s.crosshair);
    SaveDword(L"CrossStyle", s.crossStyle);
    SaveDword(L"CrossColor", s.crossColor);
    SaveDword(L"CrossSize", s.crossSize);
    WriteRegString(L"OverlayImage", s.overlayImage);
    SaveDword(L"OverlayPos", s.overlayPos);
    SaveDword(L"OverlayScale", s.overlayScale);
    SaveDword(L"OverlayOpacity", s.overlayOpacity);
    SaveDword(L"AlwaysOnTop", s.alwaysOnTop);
    SaveInputSettings(s.input);
}

// Keeps Windows from parking this process on efficiency cores / low clocks,
// which shows up as uneven frame pacing on laptops.
void BoostProcess() {
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    throttling.StateMask = 0; // 0 = never throttle
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                          &throttling, sizeof(throttling));
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

std::wstring PickFile(HWND owner, const wchar_t* filter) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::wstring(path) : std::wstring();
}

class App {
public:
    App() : frameEvent_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
            capture_(frameEvent_) {
        if (!frameEvent_) throw std::runtime_error("CreateEvent failed.");
        // One-shot waitable timer for evenly spaced 4K/30 frame presentation.
        // Prefer the high-resolution timer on Windows 10/11, but allow older
        // systems to use the regular waitable timer without failing startup.
        paceTimer_ = CreateWaitableTimerExW(nullptr, nullptr,
            0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
            TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!paceTimer_)
            paceTimer_ = CreateWaitableTimerExW(nullptr, nullptr, 0,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
    }
    void SetStartupFile(std::wstring path) { startupFile_ = std::move(path); }

    ~App() {
        // Input first (sends a neutral pad), then Remote Play (releases all
        // buttons on the console and closes the session), then video/audio.
        input_.Stop();
        remote_.Stop();
        if (cursorHidden_) ClipCursor(nullptr);
        capture_.Stop();
        audio_.Stop();
        if (paceTimer_) { CancelWaitableTimer(paceTimer_); CloseHandle(paceTimer_); }
        if (frameEvent_) CloseHandle(frameEvent_);
    }

    int Run(HINSTANCE instance) {
        instance_ = instance;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        BoostProcess();
        settings_ = LoadSettings();

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.hInstance = instance_;
        wc.lpszClassName = L"PS5DirectViewWindow";
        wc.lpfnWndProc = &App::WndProc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = CreateSolidBrush(RGB(12, 14, 18));
        wc.style = CS_HREDRAW | CS_VREDRAW;
        if (!RegisterClassExW(&wc)) throw std::runtime_error("RegisterClassEx failed.");

        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"PS5 DirectView",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1300, 780,
            nullptr, nullptr, instance_, this);
        if (!hwnd_) throw std::runtime_error("CreateWindowEx failed.");
        // Dark title bar and caption to match the UI (Windows 10 20H1+/11).
        const BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        const COLORREF caption = RGB(12, 14, 18);
        DwmSetWindowAttribute(hwnd_, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));

        renderer_.Initialize(hwnd_);
        capture_.SetDeviceManager(renderer_.DeviceManager());
        ui_.Initialize(renderer_.D2D());
        renderer_.SetOverlay([this](ID2D1DeviceContext* ctx, const D2D1_RECT_F& video) {
            ui_.Draw(ctx, video, settings_, BuildInfo());
            // WM_SETCURSOR only fires on mouse movement; once the UI fades out
            // while the mouse is idle, the pointer would otherwise linger over
            // the game. Re-apply the hide every frame.
            if (!ui_.ControlsVisible() || input_.Captured()) SetCursor(nullptr);
        });
        if (!settings_.overlayImage.empty() &&
            !ui_.LoadOverlayImage(renderer_.D2D(), settings_.overlayImage))
            settings_.overlayImage.clear();
        outputs_ = ListAudioOutputs();
        ApplyPicture();
        ApplyAudioLevels();
        audio_.SetOutput(settings_.audioOutput);
        rendererReady_ = true;

        // PS5 controller emulation: Remote Play worker + dedicated input thread.
        // Neither touches the capture or render threads.
        remote_.Start(hwnd_);
        remote_.SetAutoReconnect(settings_.input.autoReconnect);
        input_.SetSettings(settings_.input);
        input_.Start(hwnd_, [this](const PadState& pad) { remote_.SubmitPad(pad); });
        rpStatus_ = remote_.Status();
        if (settings_.input.connectOnStartup && rpStatus_.paired) remote_.Connect();

        SetTimer(hwnd_, kStatusTimer, 1000, nullptr);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        ApplyAlwaysOnTop();
        // Reopen fullscreen if it was left that way: fullscreen lets Windows
        // flip our frames straight to the display, skipping DWM composition.
        if (LoadDword(L"Fullscreen", 0)) ToggleFullscreen();
        if (!startupFile_.empty()) StartTestFile(startupFile_);
        else RefreshDevices();
        renderer_.Redraw();

        MSG message{};
        bool quit = false;
        while (!quit) {
            HANDLE handles[] = {frameEvent_, paceTimer_};
            const DWORD handleCount = paceTimer_ ? 2u : 1u;
            const DWORD wait = MsgWaitForMultipleObjectsEx(
                handleCount, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0) {
                OnFrameReady();
            } else if (paceTimer_ && wait == WAIT_OBJECT_0 + 1) {
                OnPaceDeadline();
            } else if (wait == WAIT_OBJECT_0 + handleCount) {
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) { quit = true; break; }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            } else {
                throw std::runtime_error("Window message wait failed.");
            }
        }
        return static_cast<int>(message.wParam);
    }

private:
    // ------------------------------------------------------------ devices

    void RefreshDevices() {
        std::wstring previous =
            selectedDevice_ >= 0 && selectedDevice_ < static_cast<int>(devices_.size())
                ? devices_[static_cast<size_t>(selectedDevice_)].symbolicLink : L"";
        if (previous.empty()) previous = ReadRegString(L"LastDevice");
        capture_.Stop();
        ResetPacing();
        renderer_.ResetVideo();
        frame_ = {};
        lastFrameTick_ = 0;
        devices_ = EnumerateVideoDevices();
        selectedDevice_ = -1;
        if (devices_.empty()) {
            audio_.Stop();
            Redraw();
            return;
        }
        // Last-used device first, otherwise the best-looking USB capture card.
        int choice = 0;
        int best = std::numeric_limits<int>::min();
        for (size_t i = 0; i < devices_.size(); ++i) {
            int score = CaptureDeviceScore(devices_[i]);
            if (!previous.empty() && devices_[i].symbolicLink == previous) score += 10'000;
            if (score > best) { best = score; choice = static_cast<int>(i); }
        }
        ChooseDevice(choice, false);
    }

    // Retries the capture card after unplug, sleep, or a PS5 power cycle.
    void TryReconnect() {
        if (!testFile_.empty() || capture_.Running()) return;
        const ULONGLONG now = GetTickCount64();
        if (now - lastReconnectTick_ < 3000) return;
        lastReconnectTick_ = now;
        RefreshDevices();
    }

    void ResetStats() {
        lastFrameTick_ = 0;
        fpsLastTick_ = GetTickCount64();
        fpsLastCount_ = 0;
        measuredInputFps_ = 0.0;
        lastSequence_ = 0;
        droppedFrames_ = 0;
        ResetPacing();
    }

    void ChooseDevice(int index, bool remember = true) {
        if (index < 0 || index >= static_cast<int>(devices_.size())) return;
        capture_.Stop();
        renderer_.ResetVideo();
        frame_ = {};
        ResetStats();
        selectedDevice_ = index;
        testFile_.clear();
        const std::wstring& link = devices_[static_cast<size_t>(index)].symbolicLink;
        // Explicit picks become the new default for next launch.
        if (remember) WriteRegString(L"LastDevice", link);
        capture_.Start(link, settings_.capture);
        // Card HDMI audio follows the selected video device.
        if (settings_.audioEnabled) audio_.Start(link);
        else audio_.Stop();
        Redraw();
    }

    void StartTestFile(const std::wstring& path) {
        capture_.Stop();
        renderer_.ResetVideo();
        frame_ = {};
        ResetStats();
        testFile_ = path;
        audio_.Stop();
        capture_.Start(L"FILE:" + testFile_);
        Redraw();
    }

    // ------------------------------------------------------------ settings

    void ApplyPicture() {
        // The interpolator takes ownership of frame cadence. Never pace it.
        if (!settings_.smooth4k30 || settings_.frameGeneration) ResetPacing();
        renderer_.SetFsrUpscaling(settings_.fsrUpscaling);
        renderer_.SetFrameGeneration(settings_.frameGeneration);
        renderer_.SetSharpness(kSharpLevels[settings_.sharpLevel]);
        renderer_.SetRangeOverride(settings_.rangeMode);
        renderer_.SetVSync(settings_.vsync);
    }

    void ApplyAudioLevels() {
        audio_.SetVolume(settings_.volume / 100.0f);
        audio_.SetMuted(settings_.muted);
    }

    void ApplyAlwaysOnTop() {
        SetWindowPos(hwnd_, settings_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void RestartAudio() {
        if (settings_.audioEnabled && testFile_.empty() && selectedDevice_ >= 0 &&
            selectedDevice_ < static_cast<int>(devices_.size()))
            audio_.Start(devices_[static_cast<size_t>(selectedDevice_)].symbolicLink);
        else
            audio_.Stop();
    }

    void HandleUiEvents() {
        for (const UiEvent& e : ui_.TakeEvents()) {
            switch (e.command) {
                case UiCommand::SelectDevice: ChooseDevice(e.index); break;
                case UiCommand::RefreshDevices: RefreshDevices(); break;
                case UiCommand::CaptureModeChanged:
                    if (testFile_.empty() && selectedDevice_ >= 0)
                        ChooseDevice(selectedDevice_, false);
                    break;
                case UiCommand::OpenTestVideo: {
                    const auto path = PickFile(hwnd_,
                        L"Video files (*.mp4;*.mov;*.wmv;*.mkv)\0*.mp4;*.mov;*.wmv;*.mkv\0All files\0*.*\0");
                    if (!path.empty()) StartTestFile(path);
                    break;
                }
                case UiCommand::ToggleFullscreen: ToggleFullscreen(); break;
                case UiCommand::PictureChanged: ApplyPicture(); break;
                case UiCommand::AudioToggled: RestartAudio(); break;
                case UiCommand::AudioOutputChanged: audio_.SetOutput(settings_.audioOutput); break;
                case UiCommand::VolumeChanged: ApplyAudioLevels(); break;
                case UiCommand::ChooseOverlayImage: {
                    const auto path = PickFile(hwnd_,
                        L"Images (*.png;*.jpg;*.jpeg;*.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0");
                    if (!path.empty() && ui_.LoadOverlayImage(renderer_.D2D(), path))
                        settings_.overlayImage = path;
                    break;
                }
                case UiCommand::ClearOverlayImage:
                    settings_.overlayImage.clear();
                    ui_.ClearOverlayImage();
                    break;
                case UiCommand::AlwaysOnTopChanged: ApplyAlwaysOnTop(); break;
                case UiCommand::OverlaysChanged: break;
                case UiCommand::RpConnect: remote_.Connect(); break;
                case UiCommand::RpDisconnect: remote_.Disconnect(); break;
                case UiCommand::RpDiscover: remote_.Discover(); break;
                case UiCommand::RpForget: remote_.Forget(); break;
                case UiCommand::RpPair:
                    remote_.Pair(ui_.Pair().host, ui_.Pair().account, ui_.Pair().pin);
                    ui_.ClearPin(); // single-use code: don't keep it around
                    break;
                case UiCommand::InputChanged:
                    settings_.input.Clamp();
                    input_.SetSettings(settings_.input);
                    remote_.SetAutoReconnect(settings_.input.autoReconnect);
                    if (!settings_.input.enabled) EndInputCapture();
                    break;
                case UiCommand::CaptureInput:
                    if (input_.Captured()) EndInputCapture(); else BeginInputCapture();
                    break;
            }
            settingsDirty_ = true;
        }
    }

    // ------------------------------------------------------------ frames / UI

    UiInfo BuildInfo() {
        UiInfo info;
        info.hasVideo = renderer_.HasImage() && lastFrameTick_ != 0 &&
                        GetTickCount64() - lastFrameTick_ < 1500;
        info.capturing = capture_.Running();
        info.testFile = !testFile_.empty();
        info.fullscreen = fullscreen_;
        if (selectedDevice_ >= 0 && selectedDevice_ < static_cast<int>(devices_.size()))
            info.deviceName = devices_[static_cast<size_t>(selectedDevice_)].name;
        info.signal = capture_.Signal();
        if (devices_.empty() && testFile_.empty())
            info.message = L"Plug in your USB capture card. It connects automatically.";
        else if (capture_.Running())
            info.message = L"Check the HDMI cable and that the PS5 is on.\n"
                           L"PS5: Settings \x203A System \x203A HDMI \x203A Enable HDCP: Off";
        else
            info.message = capture_.Status() + L"\nReconnecting automatically...";
        info.fps = measuredInputFps_;
        info.usbMs = pipelineLatencyMs_;
        info.appMs = appLatencyMs_;
        info.dropped = droppedFrames_;
        info.audioStatus = audio_.Status();
        info.audioPeak = audio_.Peak();
        for (const auto& d : devices_) info.devices.push_back(d.name);
        info.selectedDevice = selectedDevice_;
        info.outputs = outputs_;
        info.rp = rpStatus_;
        info.inputStats = inputStats_;
        info.pad = input_.CurrentPad();
        info.inputCaptured = input_.Captured();
        return info;
    }

    bool VideoFlowing() const {
        return lastFrameTick_ != 0 && GetTickCount64() - lastFrameTick_ < 100;
    }

    // UI changes are drawn with the next video frame (<= 16 ms away); only
    // when no video is arriving do we present on our own.
    void Redraw() {
        if (rendererReady_ && !VideoFlowing()) renderer_.Redraw();
    }

    // The HDMI capture card can deliver 4K/30 at irregular wall-clock times
    // even while the average input FPS remains 30. Present on a stable grid,
    // always taking the newest sample; never create synthetic frames.
    bool Use4kPacing(const FrameFormat& format) const {
        return settings_.smooth4k30 && paceTimer_ && !settings_.frameGeneration &&
               format.width == 3840 && format.height == 2160 &&
               format.fps >= 29.0 && format.fps <= 30.1;
    }

    void ResetPacing() {
        paceClock_.Reset();
        pacedFramePending_ = false;
        if (paceTimer_) CancelWaitableTimer(paceTimer_);
    }

    void ArmPaceTimer() {
        if (!paceTimer_ || !pacedFramePending_ || !paceClock_.Armed()) return;
        const auto remaining = paceClock_.Remaining(FramePacer::Clock::now());
        // Windows waitable timers take relative times in 100 ns units.
        const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, (nanos + 99) / 100);
        if (!SetWaitableTimer(paceTimer_, &due, 0, nullptr, nullptr, FALSE)) {
            // Timer failure must never freeze video: use legacy presentation.
            ResetPacing();
            PresentVideoFrame();
        }
    }

    void PresentVideoFrame() {
        lastFrameTick_ = GetTickCount64();
        renderer_.Render(frame_);
        // Keep the same latency / dropped-frame accounting as the original.
        const LONGLONG presented = MFGetSystemTime();
        if (frame_.receiveTime > 0) {
            const double appMs = static_cast<double>(presented - frame_.receiveTime) / 10'000.0;
            if (appMs >= 0.0 && appMs < 500.0) { appSumMs_ += appMs; ++appSamples_; }
        }
        if (frame_.sampleTime > 0 && frame_.receiveTime > 0) {
            const double usbMs = static_cast<double>(frame_.receiveTime - frame_.sampleTime) / 10'000.0;
            if (usbMs >= 0.0 && usbMs < 500.0) { latencySumMs_ += usbMs; ++latencySamples_; }
        }
        if (lastSequence_ && frame_.sequence > lastSequence_ + 1)
            droppedFrames_ += frame_.sequence - lastSequence_ - 1;
        lastSequence_ = frame_.sequence;
        frame_.buffer.Reset();
        frame_.sample.Reset();
        pacedFramePending_ = false;
    }

    void PresentPacedFrameIfDue() {
        if (!pacedFramePending_) return;
        const auto now = FramePacer::Clock::now();
        if (!paceClock_.Due(now)) { ArmPaceTimer(); return; }
        PresentVideoFrame();
        paceClock_.Presented(FramePacer::Clock::now());
    }

    void OnPaceDeadline() { PresentPacedFrameIfDue(); }

    void OnFrameReady() {
        VideoFrame incoming;
        if (!capture_.TryConsume(incoming)) { Redraw(); return; }
        lastFrameTick_ = GetTickCount64();
        if (!Use4kPacing(incoming.format)) {
            ResetPacing();
            frame_ = std::move(incoming);
            PresentVideoFrame();
            return;
        }
        const auto now = FramePacer::Clock::now();
        if (!paceClock_.Armed() || paceClock_.IsStalled(now))
            paceClock_.Start(incoming.format.fps, now);
        // Only one frame is queued: stale samples are replaced, not replayed.
        // This is deliberate: it bounds the additional video latency.
        frame_ = std::move(incoming);
        pacedFramePending_ = true;
        PresentPacedFrameIfDue();
    }

    void OnTimer() {
        const ULONGLONG now = GetTickCount64();
        const std::uint64_t frames = capture_.FramesReceived();
        if (fpsLastTick_ > 0 && now > fpsLastTick_) {
            const auto newFrames = frames >= fpsLastCount_ ? frames - fpsLastCount_ : 0;
            measuredInputFps_ = static_cast<double>(newFrames) * 1000.0 /
                                static_cast<double>(now - fpsLastTick_);
        }
        fpsLastTick_ = now;
        fpsLastCount_ = frames;
        pipelineLatencyMs_ = latencySamples_ ? latencySumMs_ / latencySamples_ : 0.0;
        appLatencyMs_ = appSamples_ ? appSumMs_ / appSamples_ : 0.0;
        latencySumMs_ = appSumMs_ = 0.0;
        latencySamples_ = appSamples_ = 0;
        if (ui_.SettingsOpen() && ++outputRefresh_ % 3 == 0) outputs_ = ListAudioOutputs();
        if (settingsDirty_) { SaveSettings(settings_); settingsDirty_ = false; }
        rpStatus_ = remote_.Status();
        inputStats_ = input_.Stats();
        wchar_t title[160];
        if (appLatencyMs_ > 0)
            swprintf_s(title, L"PS5 DirectView  \x2014  %.0f fps  \x00B7  USB %.1f ms + app %.2f ms",
                       measuredInputFps_, pipelineLatencyMs_, appLatencyMs_);
        else
            wcscpy_s(title, L"PS5 DirectView");
        std::wstring displayTitle = title;
        if (settings_.fsrUpscaling) displayTitle += L" | FSR 1 EASU";
        if (settings_.frameGeneration) displayTitle += L" | 30-in-60 interpolation (requested)";
        SetWindowTextW(hwnd_, displayTitle.c_str());
        TryReconnect();
        Redraw();
    }

    // ------------------------------------------------------------ PS5 input capture

    // Pins the (hidden) cursor to one pixel in the middle of the window so mouse
    // movement can't reach the desktop or other monitors. Raw input still
    // delivers full relative motion to the input thread.
    void ClipToWindow() {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&rc), 2);
        const LONG cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2;
        const RECT clip{cx, cy, cx + 1, cy + 1};
        SetCursorPos(cx, cy);
        ClipCursor(&clip);
    }

    void BeginInputCapture() {
        if (!settings_.input.enabled) { ui_.OpenController(); Redraw(); return; }
        if (input_.Captured()) return;
        if (GetForegroundWindow() != hwnd_) SetForegroundWindow(hwnd_);
        ui_.CloseSettings();
        ReleaseCapture(); // the click that pressed "Capture" never gets its WM_LBUTTONUP
        ClipToWindow();
        if (!cursorHidden_) { ShowCursor(FALSE); cursorHidden_ = true; }
        input_.SetCaptured(true);
        Redraw();
    }

    // Safe to call repeatedly. The input thread sends the neutral pad.
    void EndInputCapture() {
        if (input_.Captured()) input_.SetCaptured(false);
        if (!cursorHidden_) return; // nothing of ours to undo (never touch other apps' clips)
        ClipCursor(nullptr);
        ShowCursor(TRUE);
        cursorHidden_ = false;
        ui_.Activity();
        Redraw();
    }

    // Normalises WM_KEYDOWN to the left/right-specific codes bindings use.
    static InputCode KeyCode(WPARAM vk, LPARAM lparam) {
        const UINT scan = (static_cast<UINT>(lparam) >> 16) & 0xFF;
        const bool extended = (lparam & (1 << 24)) != 0;
        switch (vk) {
            case VK_SHIFT: return static_cast<InputCode>(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX));
            case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
            case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
            default: return static_cast<InputCode>(vk & 0xFF);
        }
    }

    // While a mapping slot is listening, the next key/button is bound to it.
    bool TryBind(InputCode code) {
        if (!ui_.Listening()) return false;
        if (code == VK_ESCAPE) ui_.CancelListening();
        else if (code != VK_F8 && code != 0) ui_.AssignBinding(settings_, code);
        HandleUiEvents();
        Redraw();
        return true;
    }

    void ToggleFullscreen() {
        if (!fullscreen_) {
            savedStyle_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            savedPlacement_.length = sizeof(savedPlacement_);
            GetWindowPlacement(hwnd_, &savedPlacement_);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            if (!GetMonitorInfoW(monitor, &info)) return;
            SetWindowLongPtrW(hwnd_, GWL_STYLE, (savedStyle_ & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            SetWindowPos(hwnd_, settings_.alwaysOnTop ? HWND_TOPMOST : HWND_TOP,
                         info.rcMonitor.left, info.rcMonitor.top,
                         info.rcMonitor.right - info.rcMonitor.left,
                         info.rcMonitor.bottom - info.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
            fullscreen_ = true;
        } else {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, savedStyle_);
            SetWindowPlacement(hwnd_, &savedPlacement_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen_ = false;
        }
        SaveDword(L"Fullscreen", fullscreen_ ? 1 : 0);
        Redraw();
    }

    void OnSize(unsigned width, unsigned height) {
        if (input_.Captured()) ClipToWindow();
        if (!rendererReady_ || !width || !height) return;
        renderer_.Resize(width, height);
        renderer_.Redraw();
    }

    float ToDip(int px) const { return px * 96.0f / renderer_.Dpi(); }

    void AfterInput(bool changed) {
        HandleUiEvents();
        if (changed) Redraw();
    }

    bool OnKey(WPARAM key, LPARAM lparam) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        ui_.Activity();
        if (TryBind(KeyCode(key, lparam))) return true;
        if (key == VK_F8) { BeginInputCapture(); return true; }
        if (ui_.TextFocused()) {
            ui_.OnTextKey(key, ctrl);
            HandleUiEvents();
            Redraw();
            return true;
        }
        switch (key) {
            case 'O':
                if (!ctrl) return false;
                {
                    const auto path = PickFile(hwnd_, L"Video files\0*.mp4;*.mov;*.wmv;*.mkv\0");
                    if (!path.empty()) StartTestFile(path);
                }
                return true;
            case VK_F6:
                settings_.fsrUpscaling = !settings_.fsrUpscaling;
                renderer_.SetFsrUpscaling(!renderer_.FsrUpscaling());
                SetWindowTextW(hwnd_, renderer_.FsrUpscaling()
                    ? L"PS5 DirectView | FSR 1 EASU: on" : L"PS5 DirectView | FSR 1 EASU: off");
                Redraw(); return true;
            case VK_F7:
                settings_.frameGeneration = !settings_.frameGeneration;
                renderer_.SetFrameGeneration(!renderer_.FrameGeneration());
                SetWindowTextW(hwnd_, renderer_.FrameGeneration()
                    ? L"PS5 DirectView | Motion interpolation: 30-in-60 (experimental)"
                    : L"PS5 DirectView | Motion interpolation: off");
                return true;
            case VK_F11: ToggleFullscreen(); return true;
            case VK_ESCAPE:
                if (ui_.SettingsOpen()) ui_.ToggleSettings();
                else if (fullscreen_) ToggleFullscreen();
                break;
            case VK_TAB: ui_.ToggleSettings(); break;
            case 'P': ui_.OpenController(); break;
            case VK_F5: RefreshDevices(); return true;
            case 'M': settings_.muted = !settings_.muted; ApplyAudioLevels(); break;
            case 'H': settings_.hud = !settings_.hud; break;
            case 'C': settings_.crosshair = !settings_.crosshair; break;
            case 'S': settings_.sharpLevel = (settings_.sharpLevel + 1) % 4; ApplyPicture(); break;
            case 'R': settings_.rangeMode = (settings_.rangeMode + 1) % 3; ApplyPicture(); break;
            case 'V': settings_.vsync = !settings_.vsync; ApplyPicture(); break;
            case 'A': settings_.audioEnabled = !settings_.audioEnabled; RestartAudio(); break;
            default: return false;
        }
        settingsDirty_ = true;
        Redraw();
        return true;
    }

    LRESULT Handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE: return 0;
            case WM_SIZE:
                OnSize(LOWORD(lparam), HIWORD(lparam));
                return 0;
            case WM_ERASEBKGND:
                return 1; // the swap chain covers the whole client area
            case WM_DPICHANGED: {
                renderer_.SetDpi(static_cast<float>(HIWORD(wparam)));
                const RECT* r = reinterpret_cast<const RECT*>(lparam);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left,
                             r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_MOUSEMOVE: {
                if (!trackingMouse_) {
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                    trackingMouse_ = TrackMouseEvent(&track) != FALSE;
                }
                AfterInput(ui_.MouseMove(ToDip(GET_X_LPARAM(lparam)), ToDip(GET_Y_LPARAM(lparam))));
                return 0;
            }
            case WM_MOUSELEAVE:
                trackingMouse_ = false;
                ui_.MouseMove(-1, -1);
                Redraw();
                return 0;
            case WM_LBUTTONDOWN:
                if (TryBind(input_code::kMouseLeft)) return 0;
                SetCapture(hwnd);
                AfterInput(ui_.MouseDown(ToDip(GET_X_LPARAM(lparam)), ToDip(GET_Y_LPARAM(lparam))));
                return 0;
            case WM_LBUTTONUP:
                ReleaseCapture();
                if (ui_.MouseUp()) settingsDirty_ = true;
                AfterInput(true);
                return 0;
            case WM_RBUTTONDOWN:
                if (TryBind(input_code::kMouseRight)) return 0;
                if (ui_.RightDown(ToDip(GET_X_LPARAM(lparam)), ToDip(GET_Y_LPARAM(lparam)))) {
                    HandleUiEvents();
                    Redraw();
                }
                return 0;
            case WM_MBUTTONDOWN:
                TryBind(input_code::kMouseMiddle);
                return 0;
            case WM_XBUTTONDOWN:
                TryBind(GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? input_code::kMouseX1
                                                               : input_code::kMouseX2);
                return TRUE;
            case WM_LBUTTONDBLCLK:
                ToggleFullscreen();
                return 0;
            case WM_MOUSEWHEEL: {
                if (TryBind(GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? input_code::kWheelUp
                                                               : input_code::kWheelDown))
                    return 0;
                POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(hwnd, &p);
                AfterInput(ui_.Wheel(ToDip(p.x), ToDip(p.y),
                                     static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam))));
                return 0;
            }
            case WM_KEYDOWN:
                if (OnKey(wparam, lparam)) return 0;
                break;
            case WM_CHAR:
                if (ui_.OnChar(static_cast<wchar_t>(wparam))) { Redraw(); return 0; }
                break;
            case WM_SYSKEYDOWN:
                if (TryBind(KeyCode(wparam, lparam))) return 0;
                if (wparam == VK_RETURN && (lparam & (1LL << 29))) {
                    ToggleFullscreen(); return 0;
                }
                break;
            case WM_TIMER:
                if (wparam == kStatusTimer) { OnTimer(); return 0; }
                break;
            case WM_DEVICECHANGE:
                // Capture card plugged in: connect without waiting for the timer.
                if (wparam == 0x0007 /* DBT_DEVNODES_CHANGED */) {
                    lastReconnectTick_ = 0;
                    TryReconnect();
                    if (ui_.SettingsOpen()) outputs_ = ListAudioOutputs();
                }
                break;
            case WM_ACTIVATE:
                // Focus lost (Alt+Tab, another window, lock screen): release
                // the mouse and every held controller input immediately.
                if (LOWORD(wparam) == WA_INACTIVE) EndInputCapture();
                break;
            case WM_MOVE:
            case WM_DISPLAYCHANGE:
                if (input_.Captured()) ClipToWindow();
                break;
            case InputManager::kReleasedMessage: // F8/Esc or focus loss, from the input thread
                EndInputCapture();
                return 0;
            case RemotePlayClient::kStatusMessage:
                rpStatus_ = remote_.Status();
                Redraw();
                return 0;
            case WM_SETCURSOR:
                // No mouse pointer over the game once the controls fade out.
                if (LOWORD(lparam) == HTCLIENT && (!ui_.ControlsVisible() || input_.Captured())) {
                    SetCursor(nullptr);
                    return TRUE;
                }
                break;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                KillTimer(hwnd, kStatusTimer);
                EndInputCapture();
                SaveSettings(settings_);
                PostQuitMessage(0);
                return 0;
            default: break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message,
                                     WPARAM wparam, LPARAM lparam) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            app = static_cast<App*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(hwnd, message, wparam, lparam);
        try {
            return app->Handle(hwnd, message, wparam, lparam);
        } catch (const std::exception& e) {
            const auto wide = NarrowToWide(e.what());
            MessageBoxW(hwnd, wide.c_str(), L"PS5 DirectView error", MB_OK | MB_ICONERROR);
            DestroyWindow(hwnd);
            return 0;
        }
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HANDLE frameEvent_;
    CaptureService capture_;
    D3DRenderer renderer_;
    AudioPassthrough audio_;
    Ui ui_;
    Settings settings_;
    bool settingsDirty_ = false;
    RemotePlayClient remote_;
    InputManager input_;
    RpStatus rpStatus_;
    InputStats inputStats_;
    bool cursorHidden_ = false;
    std::vector<AudioOutput> outputs_;
    unsigned outputRefresh_ = 0;
    VideoFrame frame_;
    HANDLE paceTimer_ = nullptr;
    FramePacer paceClock_;
    bool pacedFramePending_ = false;
    std::vector<VideoDevice> devices_;
    int selectedDevice_ = -1;
    std::wstring testFile_;
    std::wstring startupFile_;
    ULONGLONG lastFrameTick_ = 0;
    ULONGLONG lastReconnectTick_ = 0;
    double latencySumMs_ = 0.0;
    std::uint64_t latencySamples_ = 0;
    double pipelineLatencyMs_ = 0.0;
    double appSumMs_ = 0.0;
    std::uint64_t appSamples_ = 0;
    double appLatencyMs_ = 0.0;
    std::uint64_t lastSequence_ = 0;
    std::uint64_t droppedFrames_ = 0;
    ULONGLONG fpsLastTick_ = 0;
    std::uint64_t fpsLastCount_ = 0;
    double measuredInputFps_ = 0.0;
    bool rendererReady_ = false;
    bool fullscreen_ = false;
    bool trackingMouse_ = false;
    LONG_PTR savedStyle_ = 0;
    WINDOWPLACEMENT savedPlacement_{};
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        MessageBoxW(nullptr, L"Failed to initialize COM.", L"PS5 DirectView",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    const HRESULT mf = MFStartup(MF_VERSION);
    if (FAILED(mf)) {
        CoUninitialize();
        MessageBoxW(nullptr, L"Failed to initialize Media Foundation.",
                    L"PS5 DirectView", MB_OK | MB_ICONERROR);
        return 1;
    }
    int code = 0;
    try {
        App app;
        // "PS5DirectView.exe --file clip.mp4" opens a test video instead of USB.
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i + 1 < argc; ++i)
                if (std::wstring(argv[i]) == L"--file") app.SetStartupFile(argv[i + 1]);
            LocalFree(argv);
        }
        code = app.Run(instance);
    } catch (const std::exception& e) {
        std::wstring text = NarrowToWide(e.what());
        MessageBoxW(nullptr, text.c_str(), L"PS5 DirectView fatal error",
                    MB_OK | MB_ICONERROR);
        code = 1;
    }
    MFShutdown();
    CoUninitialize();
    return code;
}

#endif // _WIN32
