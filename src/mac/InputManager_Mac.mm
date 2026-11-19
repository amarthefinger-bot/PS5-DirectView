#include "../InputManager.h"

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <cmath>
#include <chrono>

// macOS InputManager implementation.
//
// Transport: a per-process CGEventTap at the session level. The tap sees every
// mouse and keyboard event *before* window delivery, so when captured we can
// swallow mouse motion and hot-keyed escape routes without the console ever
// seeing them as cursor events. Scroll wheel comes through as scrollWheel. We
// NEVER swallow events when not captured so the app stays usable.
//
// Scheduling: the tap and a repeating CFTimer live on a dedicated thread that
// runs its own CFRunLoop. The timer fires every 1 ms to tick the mouse->stick
// translator and watch for focus loss, matching the Windows implementation's
// cadence.
//
// Codes: Carbon kVK_* values are overlayed into the same InputCode space the
// Windows side uses (both are < 0x100). Mouse buttons use the same 0x101+
// constants.

namespace {

constexpr double kWheelPulseMs = 50.0;

double NowMs() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<double>(ns) / 1.0e6;
}

InputCode MouseButtonCode(CGMouseButton b) {
    switch (b) {
        case kCGMouseButtonLeft: return input_code::kMouseLeft;
        case kCGMouseButtonRight: return input_code::kMouseRight;
        case kCGMouseButtonCenter: return input_code::kMouseMiddle;
        default:
            // CGEventGetIntegerValueField(kCGMouseEventButtonNumber) returns 3/4 for X1/X2.
            if (b == 3) return input_code::kMouseX1;
            if (b == 4) return input_code::kMouseX2;
            return 0;
    }
}

} // namespace

struct InputManager::Impl {
    InputManager* owner = nullptr;
    NSWindow* window = nil;
    PadSink sink;
    ReleaseSink release;

    std::thread thread;
    CFRunLoopRef runLoop = nullptr;
    CFRunLoopSourceRef tapSource = nullptr;
    CFMachPortRef tap = nullptr;
    CFRunLoopTimerRef timer = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> active{false};

    std::mutex settingsMutex;
    InputSettings pending;
    InputSettings settings;
    std::atomic<bool> settingsDirty{false};

    MouseTranslator translator;
    bool down[input_code::kMax]{};
    double pressTime[input_code::kMax]{};
    double wheelUntil[2]{};
    PadState sent;
    bool sentValid = false;

    double lastTickMs = 0.0;
    std::uint64_t mouseEvents = 0, padUpdates = 0, procSamples = 0, tickSamples = 0;
    double procSum = 0.0, procMax = 0.0, tickJitterSum = 0.0;
    std::chrono::steady_clock::time_point statsStart{};

    void SetCode(InputCode code, bool pressed, double now) {
        if (code == 0 || code >= input_code::kMax) return;
        if (pressed && !down[code]) pressTime[code] = now;
        down[code] = pressed;
    }

    PadState BuildPad(double now) {
        using namespace input_code;
        auto codeActive = [&](InputCode c) {
            if (c == kNone) return false;
            if (c == kWheelUp) return now < wheelUntil[0];
            if (c == kWheelDown) return now < wheelUntil[1];
            return c < kMax && down[c];
        };
        auto actionActive = [&](Action a) {
            for (InputCode c : settings.bindings[static_cast<size_t>(a)])
                if (codeActive(c)) return true;
            return false;
        };
        auto lastPress = [&](Action a) {
            double t = -1.0;
            for (InputCode c : settings.bindings[static_cast<size_t>(a)])
                if (codeActive(c)) t = std::max(t, pressTime[c]);
            return t;
        };

        PadState pad;
        for (int i = 0; i < kPadButtonCount; ++i)
            pad.Set(static_cast<PadButton>(i), actionActive(static_cast<Action>(i)));
        pad.l2 = actionActive(Action::L2) ? 255 : 0;
        pad.r2 = actionActive(Action::R2) ? 255 : 0;

        auto axis = [&](Action negative, Action positive) {
            const double n = lastPress(negative), p = lastPress(positive);
            if (n < 0 && p < 0) return 0.0f;
            return p > n ? 1.0f : -1.0f;
        };
        float lx = axis(Action::MoveLeft, Action::MoveRight);
        float ly = axis(Action::MoveUp, Action::MoveDown);
        const float len = std::sqrt(lx * lx + ly * ly);
        if (len > 1.0f) { lx /= len; ly /= len; }
        if (actionActive(Action::Walk)) {
            const float k = settings.walkPct / 100.0f;
            lx *= k;
            ly *= k;
        }
        pad.lx = StickAxis(lx);
        pad.ly = StickAxis(ly);

        const StickVector r = translator.Evaluate(now);
        pad.rx = StickAxis(r.x);
        pad.ry = StickAxis(r.y);
        return pad;
    }

    void Evaluate(bool timed) {
        const auto t0 = std::chrono::steady_clock::now();
        const double now = NowMs();
        const PadState pad = BuildPad(now);
        const bool changed = !sentValid || pad != sent;
        if (changed && sink) sink(pad);
        if (changed) { sent = pad; sentValid = true; }
        const auto t1 = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(owner->statsMutex_);
        owner->current_ = pad;
        if (changed) ++padUpdates;
        if (!timed) {
            const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            procSum += us;
            procMax = std::max(procMax, us);
            ++procSamples;
        } else {
            const double late = now - lastTickMs - 1.0;
            if (late >= 0.0 && late < 100.0) { tickJitterSum += late * 1000.0; ++tickSamples; }
            lastTickMs = now;
        }
        const double elapsed = std::chrono::duration<double>(t1 - statsStart).count();
        if (elapsed >= 1.0) {
            owner->stats_.mouseHz = mouseEvents / elapsed;
            owner->stats_.padUpdatesPerSec = padUpdates / elapsed;
            owner->stats_.processAvgUs = procSamples ? procSum / procSamples : 0.0;
            owner->stats_.processMaxUs = procMax;
            owner->stats_.tickJitterUs = tickSamples ? tickJitterSum / tickSamples : 0.0;
            mouseEvents = padUpdates = procSamples = tickSamples = 0;
            procSum = procMax = tickJitterSum = 0.0;
            statsStart = t1;
        }
    }

    void ReleaseAll(bool notify, int reason) {
        owner->captured_ = false;
        ApplyCapture(false);
        if (notify && release) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (release) release(reason);
            });
        }
    }

    void ApplyCapture(bool capture) {
        if (capture == active.load()) return;
        if (capture) {
            std::fill(std::begin(down), std::end(down), false);
            wheelUntil[0] = wheelUntil[1] = 0.0;
            translator.Reset();
            sentValid = false;
            active = true;
            lastTickMs = NowMs();
            // Hide system cursor at capture scope. CGDisplayHideCursor must
            // be balanced by a matching CGDisplayShowCursor.
            dispatch_async(dispatch_get_main_queue(), ^{
                CGDisplayHideCursor(kCGDirectMainDisplay);
                CGAssociateMouseAndMouseCursorPosition(FALSE);
            });
        } else {
            active = false;
            std::fill(std::begin(down), std::end(down), false);
            translator.Reset();
            const PadState neutral{};
            if (sink) sink(neutral);
            sent = neutral;
            sentValid = true;
            {
                std::lock_guard<std::mutex> lock(owner->statsMutex_);
                owner->current_ = neutral;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                CGAssociateMouseAndMouseCursorPosition(TRUE);
                CGDisplayShowCursor(kCGDirectMainDisplay);
            });
        }
    }

    CGEventRef HandleEvent(CGEventType type, CGEventRef event) {
        if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
            if (tap) CGEventTapEnable(tap, true);
            return event;
        }
        if (!active.load()) return event;

        const double now = NowMs();
        bool swallow = false;

        switch (type) {
            case kCGEventMouseMoved:
            case kCGEventLeftMouseDragged:
            case kCGEventRightMouseDragged:
            case kCGEventOtherMouseDragged: {
                const int64_t dx = CGEventGetIntegerValueField(event, kCGMouseEventDeltaX);
                const int64_t dy = CGEventGetIntegerValueField(event, kCGMouseEventDeltaY);
                if (dx || dy) {
                    translator.AddMotion(now, static_cast<int>(dx), static_cast<int>(dy));
                    ++mouseEvents;
                }
                swallow = true;
                break;
            }
            case kCGEventLeftMouseDown:   SetCode(input_code::kMouseLeft,   true,  now); swallow = true; break;
            case kCGEventLeftMouseUp:     SetCode(input_code::kMouseLeft,   false, now); swallow = true; break;
            case kCGEventRightMouseDown:  SetCode(input_code::kMouseRight,  true,  now); swallow = true; break;
            case kCGEventRightMouseUp:    SetCode(input_code::kMouseRight,  false, now); swallow = true; break;
            case kCGEventOtherMouseDown: {
                const int64_t b = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
                SetCode(MouseButtonCode(static_cast<CGMouseButton>(b)), true, now);
                swallow = true;
                break;
            }
            case kCGEventOtherMouseUp: {
                const int64_t b = CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber);
                SetCode(MouseButtonCode(static_cast<CGMouseButton>(b)), false, now);
                swallow = true;
                break;
            }
            case kCGEventScrollWheel: {
                const int64_t delta = CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1);
                if (delta > 0) { wheelUntil[0] = now + kWheelPulseMs; pressTime[input_code::kWheelUp] = now; }
                if (delta < 0) { wheelUntil[1] = now + kWheelPulseMs; pressTime[input_code::kWheelDown] = now; }
                swallow = true;
                break;
            }
            case kCGEventKeyDown:
            case kCGEventKeyUp: {
                const int64_t keyCode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
                const bool pressed = (type == kCGEventKeyDown);
                // Escape = quick release hotkey (kVK_Escape = 0x35).
                if (keyCode == 0x35 && pressed) {
                    ReleaseAll(true, InputManager::kReleasedByKey);
                    return nullptr;
                }
                if (keyCode >= 0 && keyCode < input_code::kMax)
                    SetCode(static_cast<InputCode>(keyCode), pressed, now);
                swallow = true;
                break;
            }
            case kCGEventFlagsChanged: {
                // Modifier keys arrive as flagsChanged. Deduce up/down from the mask.
                const int64_t keyCode = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
                const CGEventFlags flags = CGEventGetFlags(event);
                CGEventFlags mask = 0;
                switch (keyCode) {
                    case 0x38: case 0x3C: mask = kCGEventFlagMaskShift; break;       // L/R Shift
                    case 0x3B: case 0x3E: mask = kCGEventFlagMaskControl; break;     // L/R Control
                    case 0x3A: case 0x3D: mask = kCGEventFlagMaskAlternate; break;   // L/R Option
                    case 0x37: case 0x36: mask = kCGEventFlagMaskCommand; break;     // L/R Command
                    default: break;
                }
                const bool pressed = mask && (flags & mask);
                if (keyCode >= 0 && keyCode < input_code::kMax)
                    SetCode(static_cast<InputCode>(keyCode), pressed, now);
                swallow = true;
                break;
            }
            default:
                break;
        }

        if (swallow) {
            Evaluate(false);
            return nullptr;
        }
        return event;
    }

    static CGEventRef TapCallback(CGEventTapProxy, CGEventType type,
                                  CGEventRef event, void* userInfo) {
        return static_cast<Impl*>(userInfo)->HandleEvent(type, event);
    }

    static void TimerCallback(CFRunLoopTimerRef, void* userInfo) {
        Impl* self = static_cast<Impl*>(userInfo);
        if (!self->active.load()) return;
        if (self->settingsDirty.exchange(false)) {
            std::lock_guard<std::mutex> lock(self->settingsMutex);
            self->settings = self->pending;
            self->translator.Configure(self->settings.aim);
        }
        // Focus check: give up capture when app loses key window.
        __block bool focused = false;
        if ([NSThread isMainThread]) {
            focused = [NSApp isActive] && self->window.isKeyWindow;
        } else {
            dispatch_sync(dispatch_get_main_queue(), ^{
                focused = [NSApp isActive] && self->window.isKeyWindow;
            });
        }
        if (!focused) {
            self->ReleaseAll(true, InputManager::kReleasedByFocus);
            return;
        }
        self->Evaluate(true);
    }

    void ThreadMain() {
        runLoop = CFRunLoopGetCurrent();
        // Session-wide tap so we see events even when the app window isn't key.
        // kCGHeadInsertEventTap puts us first in the chain (lets us swallow).
        const CGEventMask mask =
            CGEventMaskBit(kCGEventMouseMoved) |
            CGEventMaskBit(kCGEventLeftMouseDragged) |
            CGEventMaskBit(kCGEventRightMouseDragged) |
            CGEventMaskBit(kCGEventOtherMouseDragged) |
            CGEventMaskBit(kCGEventLeftMouseDown) | CGEventMaskBit(kCGEventLeftMouseUp) |
            CGEventMaskBit(kCGEventRightMouseDown) | CGEventMaskBit(kCGEventRightMouseUp) |
            CGEventMaskBit(kCGEventOtherMouseDown) | CGEventMaskBit(kCGEventOtherMouseUp) |
            CGEventMaskBit(kCGEventScrollWheel) |
            CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp) |
            CGEventMaskBit(kCGEventFlagsChanged);
        tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                               kCGEventTapOptionDefault, mask, &TapCallback, this);
        if (tap) {
            tapSource = CFMachPortCreateRunLoopSource(nullptr, tap, 0);
            CFRunLoopAddSource(runLoop, tapSource, kCFRunLoopCommonModes);
            CGEventTapEnable(tap, true);
        }

        CFRunLoopTimerContext ctx{0, this, nullptr, nullptr, nullptr};
        timer = CFRunLoopTimerCreate(nullptr,
            CFAbsoluteTimeGetCurrent() + 0.001, 0.001,
            0, 0, &TimerCallback, &ctx);
        if (timer) CFRunLoopAddTimer(runLoop, timer, kCFRunLoopCommonModes);

        statsStart = std::chrono::steady_clock::now();
        running = true;
        while (running.load()) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
        }

        if (active.load()) ApplyCapture(false);
        if (timer) {
            CFRunLoopRemoveTimer(runLoop, timer, kCFRunLoopCommonModes);
            CFRunLoopTimerInvalidate(timer);
            CFRelease(timer);
            timer = nullptr;
        }
        if (tap) {
            CGEventTapEnable(tap, false);
            if (tapSource) {
                CFRunLoopRemoveSource(runLoop, tapSource, kCFRunLoopCommonModes);
                CFRelease(tapSource);
                tapSource = nullptr;
            }
            CFRelease(tap);
            tap = nullptr;
        }
        runLoop = nullptr;
    }
};

InputManager::InputManager() = default;
InputManager::~InputManager() { Stop(); }

void InputManager::Start(void* nsWindow, PadSink sink, ReleaseSink onRelease) {
    if (impl_) return;
    impl_ = std::make_unique<Impl>();
    impl_->owner = this;
    impl_->window = (__bridge NSWindow*)nsWindow;
    impl_->sink = std::move(sink);
    impl_->release = std::move(onRelease);
    {
        std::lock_guard<std::mutex> lock(impl_->settingsMutex);
        impl_->settings = impl_->pending;
    }
    impl_->translator.Configure(impl_->settings.aim);
    impl_->thread = std::thread([this] { impl_->ThreadMain(); });
    // Spin briefly until the run loop is ready so SetSettings/SetCaptured can
    // post to it immediately.
    for (int i = 0; i < 500 && !impl_->running.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void InputManager::Stop() {
    if (!impl_) return;
    impl_->running = false;
    if (impl_->runLoop) CFRunLoopWakeUp(impl_->runLoop);
    if (impl_->thread.joinable()) impl_->thread.join();
    captured_ = false;
    impl_.reset();
}

void InputManager::SetSettings(const InputSettings& settings) {
    if (!impl_) {
        std::lock_guard<std::mutex> lock(statsMutex_);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->settingsMutex);
        impl_->pending = settings;
    }
    impl_->settingsDirty = true;
    if (impl_->runLoop) CFRunLoopWakeUp(impl_->runLoop);
}

void InputManager::SetCaptured(bool capture) {
    captured_ = capture;
    if (!impl_) return;
    // Jump to the input thread so ApplyCapture runs there (and only there).
    Impl* impl = impl_.get();
    CFRunLoopRef loop = impl->runLoop;
    if (!loop) return;
    CFRunLoopPerformBlock(loop, kCFRunLoopCommonModes, ^{
        impl->ApplyCapture(capture);
    });
    CFRunLoopWakeUp(loop);
}

PadState InputManager::CurrentPad() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return current_;
}

InputStats InputManager::Stats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}
