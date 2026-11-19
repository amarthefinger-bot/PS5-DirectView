// Offline tests for mouse/keyboard -> controller translation (no PS5 needed).
// Build: cmake --build build --config Release --target InputTests
#include "ControllerState.h"
#include "InputSettings.h"
#include "MouseTranslator.h"

#ifdef _WIN32
#include <windows.h>
#else
// Carbon kVK_* values (same values InputSettings.cpp uses in mac_vk).
// Reused here so the test can name default bindings portably.
#define VK_LSHIFT   0x38
#define VK_LCONTROL 0x3B
#define VK_SPACE    0x31
#define VK_F8       0x64
#define VK_ESCAPE   0x35
#endif

#include <chrono>
#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what); }
}

bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

AimSettings Plain() {
    AimSettings s;
    s.sensitivity = 50;     // gain 0.2 per count/ms
    s.verticalPct = 100;
    s.deadzonePct = 0;
    s.antiDeadzonePct = 0;
    s.curve = 0;
    s.accelPct = 0;
    s.smoothingMs = 0;
    s.windowMs = 8;
    s.recenterMs = 10;
    return s;
}

// Feeds a 1000 Hz mouse moving (dx, dy) counts per report for `ms` ms,
// evaluating every ms like the input thread. Returns the last output.
StickVector Drive(MouseTranslator& t, double& now, int dx, int dy, int ms) {
    StickVector out;
    for (int i = 0; i < ms; ++i) {
        now += 1.0;
        t.AddMotion(now, dx, dy);
        out = t.Evaluate(now);
    }
    return out;
}
} // namespace

int main() {
    // --- defaults required by the spec
    {
        const Bindings b = DefaultBindings();
        auto first = [&](Action a) { return b[static_cast<size_t>(a)][0]; };
        Check(first(Action::MoveUp) == 'W' && first(Action::MoveLeft) == 'A' &&
              first(Action::MoveDown) == 'S' && first(Action::MoveRight) == 'D', "WASD -> left stick");
        Check(first(Action::L3) == VK_LSHIFT, "Shift -> L3");
        Check(first(Action::Cross) == VK_SPACE, "Space -> Cross");
        Check(first(Action::Square) == 'R', "R -> Square");
        Check(first(Action::Triangle) == 'E', "E -> Triangle");
        Check(first(Action::Circle) == VK_LCONTROL, "Ctrl -> Circle");
        Check(first(Action::L1) == 'Q', "Q -> L1");
        Check(first(Action::R2) == input_code::kMouseLeft, "Left mouse -> R2");
        Check(first(Action::L2) == input_code::kMouseRight, "Right mouse -> L2");
        bool noRelease = true;
        for (const auto& slots : b)
            for (InputCode c : slots) if (c == VK_F8 || c == VK_ESCAPE) noRelease = false;
        Check(noRelease, "F8/Esc are never bound");
    }

    // --- stick axis conversion
    Check(StickAxis(0.0f) == 0 && StickAxis(1.0f) == 32767 && StickAxis(-1.0f) == -32767, "StickAxis range");
    Check(StickAxis(2.0f) == 32767 && StickAxis(-5.0f) == -32767, "StickAxis saturates");

    // --- no motion -> centred
    {
        MouseTranslator t;
        t.Configure(Plain());
        const StickVector v = t.Evaluate(5.0);
        Check(v.x == 0.0f && v.y == 0.0f, "idle stick is centred");
    }

    // --- steady motion: deflection = speed * gain (linear, no extras)
    {
        MouseTranslator t;
        t.Configure(Plain());
        double now = 0;
        const StickVector v = Drive(t, now, 2, 0, 50); // 2 counts/ms
        Check(Near(v.x, 0.4f, 0.01f) && Near(v.y, 0.0f), "linear: 2 counts/ms -> 0.4 at sens 50");
        const StickVector d = Drive(t, now, 0, 3, 50);  // moving down
        Check(d.y > 0.55f && Near(d.x, 0.0f, 0.01f), "mouse down -> stick down (+y)");
    }

    // --- recentering after motion stops
    {
        AimSettings s = Plain();
        s.recenterMs = 10;
        MouseTranslator t;
        t.Configure(s);
        double now = 0;
        Drive(t, now, 3, 0, 30);
        StickVector v{};
        for (int i = 0; i < 11; ++i) { now += 1; v = t.Evaluate(now); }
        Check(v.x == 0.0f && v.y == 0.0f, "stick recentres exactly after recenterMs");
    }

    // --- circular stick limit
    {
        MouseTranslator t;
        t.Configure(Plain());
        double now = 0;
        const StickVector v = Drive(t, now, 400, 400, 20);
        Check(std::sqrt(v.x * v.x + v.y * v.y) <= 1.0001f, "never beyond full deflection");
        Check(Near(v.x, v.y, 1e-3f), "diagonal stays diagonal when clamped");
    }

    // --- anti-deadzone & deadzone
    {
        AimSettings s = Plain();
        s.antiDeadzonePct = 20;
        Check(Near(MouseTranslator::Shape(0.0f, s), 0.0f), "anti-deadzone: zero stays zero");
        Check(MouseTranslator::Shape(0.01f, s) >= 0.2f, "anti-deadzone: tiny motion jumps past game deadzone");
        Check(Near(MouseTranslator::Shape(1.0f, s), 1.0f), "anti-deadzone keeps full range");
        s.antiDeadzonePct = 0;
        s.deadzonePct = 10;
        Check(MouseTranslator::Shape(0.05f, s) == 0.0f, "deadzone filters small motion");
        Check(Near(MouseTranslator::Shape(0.55f, s), 0.5f), "deadzone rescales remaining range");
    }

    // --- curves and acceleration
    {
        AimSettings s = Plain();
        s.curve = 1;
        s.exponentPct = 200;
        Check(Near(MouseTranslator::Shape(0.5f, s), 0.25f), "power curve exponent 2");
        s.exponentPct = 100;
        Check(Near(MouseTranslator::Shape(0.5f, s), 0.5f), "exponent 1.0 is linear");
        s.curve = 0;
        s.accelPct = 100;
        Check(Near(MouseTranslator::Shape(0.5f, s), 0.75f), "acceleration adds d^2");
        Check(Near(MouseTranslator::Shape(0.9f, s), 1.0f), "acceleration respects the stick limit");
    }

    // --- vertical sensitivity & invert
    {
        AimSettings s = Plain();
        s.verticalPct = 50;
        MouseTranslator t;
        t.Configure(s);
        double now = 0;
        const StickVector v = Drive(t, now, 0, 2, 40);
        Check(Near(v.y, 0.2f, 0.01f), "vertical sensitivity 50%");
        s.verticalPct = 100;
        s.invertY = true;
        t.Configure(s);
        t.Reset();
        now = 0;
        const StickVector w = Drive(t, now, 0, 2, 40);
        Check(w.y < 0.0f, "invert Y");
    }

    // --- smoothing: 0 = immediate, >0 = gradual
    {
        MouseTranslator a, b;
        AimSettings s = Plain();
        a.Configure(s);
        s.smoothingMs = 20;
        b.Configure(s);
        double ta = 0, tb = 0;
        // 8 ms of warm-up fills the speed window.
        const StickVector va = Drive(a, ta, 2, 0, 8);
        const StickVector vb = Drive(b, tb, 2, 0, 8);
        Check(Near(va.x, 0.4f, 0.01f), "zero smoothing reaches target within the speed window");
        Check(vb.x < va.x * 0.6f, "smoothing delays the response");
    }

    // --- 8 kHz mouse doesn't overflow the window buffer
    {
        MouseTranslator t;
        AimSettings s = Plain();
        s.windowMs = 32;
        t.Configure(s);
        double now = 0;
        StickVector v;
        for (int i = 0; i < 8 * 200; ++i) { now += 0.125; t.AddMotion(now, 1, 0); v = t.Evaluate(now); }
        Check(Near(v.x, 1.0f, 0.02f), "8 kHz: 8 counts/ms saturates at sens 50");
    }

    // --- timing: translator cost per evaluation
    {
        MouseTranslator t;
        AimSettings s = Plain();
        s.curve = 1;
        s.exponentPct = 150;
        s.smoothingMs = 5;
        t.Configure(s);
        const int n = 1'000'000;
        double now = 0;
        volatile float sink = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) {
            now += 0.125;
            t.AddMotion(now, (i % 7) - 3, (i % 5) - 2);
            sink = sink + t.Evaluate(now).x;
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / n;
        std::printf("translator AddMotion+Evaluate: %.1f ns per call\n", ns);
        Check(ns < 50'000.0, "translator well under 1 ms per update");
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
