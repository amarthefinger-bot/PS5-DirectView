#pragma once

#include "Audio.h"
#include "InputManager.h"
#include "InputSettings.h"
#include "RemotePlayClient.h"

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>

// Everything the user can configure. Persisted by the app in the registry.
struct Settings {
    int sharpLevel = 2;        // 0 off .. 3 high
    int rangeMode = 0;         // 0 auto, 1 full, 2 limited
    bool vsync = false;
    bool audioEnabled = true;
    std::wstring audioOutput;  // empty = Windows default
    int volume = 100;          // 0..100
    bool muted = false;
    bool hud = false;          // performance overlay
    bool crosshair = false;
    int crossStyle = 0;        // 0 cross, 1 gap cross, 2 dot, 3 circle
    int crossColor = 0;        // index into the swatch palette
    int crossSize = 14;        // DIPs
    std::wstring overlayImage; // PNG/JPG drawn over the game
    int overlayPos = 0;        // 0 fill, 1 top-left, 2 top-right, 3 bottom-left, 4 bottom-right
    int overlayScale = 25;     // % of video width (corner placement)
    int overlayOpacity = 100;  // %
    bool alwaysOnTop = false;
    InputSettings input;       // mouse & keyboard -> PS5 controller (saved separately)
};

// Live state the UI displays (read-only for the UI).
struct UiInfo {
    bool hasVideo = false;
    bool capturing = false;
    bool testFile = false;
    bool fullscreen = false;
    std::wstring deviceName;
    std::wstring signal;       // "1920x1080 · 60 fps · NV12 · GPU"
    std::wstring message;      // shown on the no-signal screen
    double fps = 0, usbMs = 0, appMs = 0;
    unsigned long long dropped = 0;
    std::wstring audioStatus;
    float audioPeak = 0;
    std::vector<std::wstring> devices;
    int selectedDevice = -1;
    std::vector<AudioOutput> outputs;
    // Controller (PS5 Remote Play input).
    RpStatus rp;
    InputStats inputStats;
    PadState pad;
    bool inputCaptured = false;
};

enum class UiCommand {
    SelectDevice, RefreshDevices, OpenTestVideo, ToggleFullscreen,
    PictureChanged, AudioToggled, AudioOutputChanged, VolumeChanged,
    ChooseOverlayImage, ClearOverlayImage, AlwaysOnTopChanged, OverlaysChanged,
    // Controller tab.
    RpConnect, RpDisconnect, RpDiscover, RpPair, RpForget, InputChanged, CaptureInput,
};

// Text typed into the pairing form (owned by the Ui).
struct PairForm {
    std::wstring host, account, pin;
};

struct UiEvent {
    UiCommand command;
    int index = 0;
};

// Immediate-mode UI drawn with Direct2D into the video swap chain.
// All coordinates are DIPs.
class Ui {
public:
    void Initialize(ID2D1DeviceContext* context);
    void Draw(ID2D1DeviceContext* context, const D2D1_RECT_F& video,
              Settings& settings, const UiInfo& info);

    bool MouseMove(float x, float y);
    bool MouseDown(float x, float y);
    bool MouseUp();
    bool Wheel(float x, float y, float delta);
    void Activity() { lastActivity_ = GetTickCount64(); }
    bool ControlsVisible() const { return fade_ > 0.01f; }
    bool SettingsOpen() const { return settingsOpen_; }
    void ToggleSettings() { settingsOpen_ = !settingsOpen_; Activity(); }
    void OpenController() { settingsOpen_ = true; tab_ = 1; Activity(); }
    void CloseSettings() { settingsOpen_ = false; focusedField_ = -1; listenAction_ = -1; }
    bool RightDown(float x, float y);

    // Text fields (pairing form).
    bool TextFocused() const { return settingsOpen_ && focusedField_ >= 0; }
    bool OnChar(wchar_t ch);
    bool OnTextKey(WPARAM key, bool ctrl);
    const PairForm& Pair() const { return pair_; }
    void ClearPin() { pair_.pin.clear(); }

    // Binding capture: after clicking a mapping slot, the next key / mouse
    // button pressed on the main window is assigned to it.
    bool Listening() const { return settingsOpen_ && listenAction_ >= 0; }
    void AssignBinding(Settings& settings, InputCode code);
    void CancelListening() { listenAction_ = -1; }
    std::vector<UiEvent> TakeEvents();

    bool LoadOverlayImage(ID2D1DeviceContext* context, const std::wstring& path);
    void ClearOverlayImage() { overlayBitmap_.Reset(); }

private:
    struct Hit {
        D2D1_RECT_F rect;
        D2D1_RECT_F clip;
        std::function<void()> click;
        std::function<void(float)> drag;
        std::function<void()> rightClick;
    };

    // Drawing helpers.
    void Fill(const D2D1_RECT_F& r, UINT32 rgb, float alpha = 1.0f, float radius = 0.0f);
    void Stroke(const D2D1_RECT_F& r, UINT32 rgb, float alpha, float radius, float width = 1.0f);
    void Text(const std::wstring& text, const D2D1_RECT_F& r, IDWriteTextFormat* format,
              UINT32 rgb, float alpha = 1.0f,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    float Measure(const std::wstring& text, IDWriteTextFormat* format);
    void Icon(wchar_t glyph, const D2D1_RECT_F& r, UINT32 rgb, float alpha = 1.0f,
              IDWriteTextFormat* format = nullptr);
    bool Hover(const D2D1_RECT_F& r) const;
    void AddHit(const D2D1_RECT_F& r, std::function<void()> click,
                std::function<void(float)> drag = nullptr);

    // Widgets.
    void IconButton(const D2D1_RECT_F& r, wchar_t glyph, bool active,
                    std::function<void()> click, const wchar_t* tip = nullptr);
    void Button(const D2D1_RECT_F& r, const std::wstring& label, wchar_t glyph,
                std::function<void()> click, bool primary = false);
    float SectionTitle(float y, const std::wstring& title);
    float ToggleRow(float y, const std::wstring& label, const std::wstring& hint,
                    bool& value, std::function<void()> changed);
    float Segmented(float y, const std::wstring& label, const std::vector<std::wstring>& options,
                    int& value, std::function<void()> changed);
    float SliderRow(float y, const std::wstring& label, int& value, int minimum, int maximum,
                    const wchar_t* unit, std::function<void()> changed);
    void Slider(const D2D1_RECT_F& track, int& value, int minimum, int maximum,
                std::function<void()> changed);
    float RadioRow(float y, const std::wstring& label, bool selected,
                   std::function<void()> click);
    float TextField(float y, const std::wstring& label, std::wstring& value, int id,
                    const wchar_t* placeholder);
    float Paragraph(float y, const std::wstring& text, UINT32 rgb, float height);
    void StickView(const D2D1_RECT_F& r, float x, float y, const wchar_t* label);

    // Screens.
    void DrawOverlays(const D2D1_RECT_F& video, const Settings& s, const UiInfo& info);
    void DrawNoSignal(const D2D1_SIZE_F& size, const UiInfo& info);
    void DrawTopBar(const D2D1_SIZE_F& size, Settings& s, const UiInfo& info);
    void DrawBottomBar(const D2D1_SIZE_F& size, Settings& s, const UiInfo& info);
    void DrawSettings(const D2D1_SIZE_F& size, Settings& s, const UiInfo& info);
    float DrawVideoTab(float y, Settings& s, const UiInfo& info);
    float DrawControllerTab(float y, Settings& s, const UiInfo& info);
    void DrawCaptureToast(const D2D1_SIZE_F& size, const UiInfo& info);

    ID2D1DeviceContext* ctx_ = nullptr;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title_, body_, bodyBold_, small_, caption_,
        big_, icon_, iconLarge_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> overlayBitmap_;

    std::vector<Hit> hits_;
    std::vector<UiEvent> events_;
    std::function<void(float)> activeDrag_;
    D2D1_RECT_F clip_{};
    float mouseX_ = -1, mouseY_ = -1;
    float fade_ = 1.0f;      // controls opacity (auto-hide)
    float alpha_ = 1.0f;     // multiplier applied by the drawing helpers
    float panelX_ = 0;       // left edge of the settings drawer
    float scroll_ = 0, contentHeight_ = 0, viewHeight_ = 0;
    bool settingsOpen_ = false;
    ULONGLONG lastActivity_ = 0;
    int tab_ = 0;               // 0 video & audio, 1 controller
    int focusedField_ = -1;     // pairing text field with keyboard focus
    int listenAction_ = -1, listenSlot_ = 0;
    bool showPairForm_ = false;
    PairForm pair_;
    bool wasCaptured_ = false;
    ULONGLONG capturedTick_ = 0;
};
