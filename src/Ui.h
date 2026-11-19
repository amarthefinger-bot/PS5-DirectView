#pragma once

#include "Audio.h"
#include "InputManager.h"
#include "InputSettings.h"
#include "RemotePlayClient.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
  #include <d2d1_1.h>
  #include <dwrite.h>
  #include <wrl/client.h>
#endif

#ifdef _WIN32
using UiStringT = std::wstring;
#else
using UiStringT = std::string;
#endif

struct Settings {
    int sharpLevel = 2;
    int rangeMode = 0;
    bool vsync = false;
    bool audioEnabled = true;
    UiStringT audioOutput;
    int volume = 100;
    bool muted = false;
    bool hud = false;
    bool crosshair = false;
    int crossStyle = 0;
    int crossColor = 0;
    int crossSize = 14;
    UiStringT overlayImage;
    int overlayPos = 0;
    int overlayScale = 25;
    int overlayOpacity = 100;
    bool alwaysOnTop = false;
    InputSettings input;
};

struct UiInfo {
    bool hasVideo = false;
    bool capturing = false;
    bool testFile = false;
    bool fullscreen = false;
    UiStringT deviceName;
    UiStringT signal;
    UiStringT message;
    double fps = 0, usbMs = 0, appMs = 0;
    unsigned long long dropped = 0;
    UiStringT audioStatus;
    float audioPeak = 0;
    std::vector<UiStringT> devices;
    int selectedDevice = -1;
    std::vector<AudioOutput> outputs;
    RpStatus rp;
    InputStats inputStats;
    PadState pad;
    bool inputCaptured = false;
};

enum class UiCommand {
    SelectDevice, RefreshDevices, OpenTestVideo, ToggleFullscreen,
    PictureChanged, AudioToggled, AudioOutputChanged, VolumeChanged,
    ChooseOverlayImage, ClearOverlayImage, AlwaysOnTopChanged, OverlaysChanged,
    RpConnect, RpDisconnect, RpDiscover, RpPair, RpForget, InputChanged, CaptureInput,
};

struct PairForm {
    UiStringT host, account, pin;
};

struct UiEvent {
    UiCommand command;
    int index = 0;
};

#ifdef _WIN32

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

    bool TextFocused() const { return settingsOpen_ && focusedField_ >= 0; }
    bool OnChar(wchar_t ch);
    bool OnTextKey(WPARAM key, bool ctrl);
    const PairForm& Pair() const { return pair_; }
    void ClearPin() { pair_.pin.clear(); }

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
    float fade_ = 1.0f;
    float alpha_ = 1.0f;
    float panelX_ = 0;
    float scroll_ = 0, contentHeight_ = 0, viewHeight_ = 0;
    bool settingsOpen_ = false;
    ULONGLONG lastActivity_ = 0;
    int tab_ = 0;
    int focusedField_ = -1;
    int listenAction_ = -1, listenSlot_ = 0;
    bool showPairForm_ = false;
    PairForm pair_;
    bool wasCaptured_ = false;
    ULONGLONG capturedTick_ = 0;
};

#else

// macOS Ui: hosts a native NSMenu + a borderless overlay NSWindow that draws
// the DirectView controls with CoreGraphics on top of the Metal view.
// Draw() is called once per frame from Renderer_Mac.mm's overlay callback.
class Ui {
public:
    using RectF = struct { float left, top, right, bottom; };
    using SizeF = struct { float width, height; };

    // ctx: CGContextRef bridged as void*.
    void Initialize(void* cgContext);
    void Draw(void* cgContext, const RectF& video, Settings& settings, const UiInfo& info);

    bool MouseMove(float x, float y);
    bool MouseDown(float x, float y);
    bool MouseUp();
    bool Wheel(float x, float y, float delta);
    void Activity();
    bool ControlsVisible() const;
    bool SettingsOpen() const;
    void ToggleSettings();
    void OpenController();
    void CloseSettings();
    bool RightDown(float x, float y);

    bool TextFocused() const;
    bool OnChar(uint32_t ch);     // UTF-32 codepoint
    bool OnTextKey(int keyCode, bool ctrl);
    const PairForm& Pair() const { return pair_; }
    void ClearPin() { pair_.pin.clear(); }

    bool Listening() const;
    void AssignBinding(Settings& settings, InputCode code);
    void CancelListening();
    std::vector<UiEvent> TakeEvents();

    bool LoadOverlayImage(void* cgContext, const std::string& path);
    void ClearOverlayImage();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    PairForm pair_;
    std::vector<UiEvent> events_;
    friend struct Impl;
};

#endif
