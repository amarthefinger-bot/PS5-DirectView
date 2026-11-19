#include "../Ui.h"

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cstdio>

// macOS Ui: a thin overlay. All user commands land via the native menu bar
// (see MacApp.mm) which drives this Settings struct. This class is responsible
// only for the HUD readout, crosshair, overlay image, and capture toast.

namespace {

struct RGBA { float r, g, b, a; };

constexpr RGBA kWhite = {1.0f, 1.0f, 1.0f, 1.0f};
constexpr RGBA kBlack = {0.0f, 0.0f, 0.0f, 1.0f};

RGBA PanelBG() { return {0.07f, 0.08f, 0.1f, 0.78f}; }
RGBA Accent() { return {0.31f, 0.78f, 1.0f, 1.0f}; }

RGBA CrossColor(int idx) {
    switch (idx) {
        case 0: return {1.0f, 1.0f, 1.0f, 0.9f};
        case 1: return {1.0f, 0.2f, 0.2f, 0.9f};
        case 2: return {0.2f, 1.0f, 0.3f, 0.9f};
        case 3: return {1.0f, 0.86f, 0.1f, 0.9f};
        case 4: return {0.4f, 0.7f, 1.0f, 0.9f};
        default: return {0.9f, 0.3f, 0.9f, 0.9f};
    }
}

void SetFill(CGContextRef ctx, RGBA c) {
    CGContextSetRGBFillColor(ctx, c.r, c.g, c.b, c.a);
}

void SetStroke(CGContextRef ctx, RGBA c) {
    CGContextSetRGBStrokeColor(ctx, c.r, c.g, c.b, c.a);
}

void FillRoundedRect(CGContextRef ctx, CGRect r, CGFloat radius, RGBA color) {
    SetFill(ctx, color);
    if (radius <= 0) { CGContextFillRect(ctx, r); return; }
    CGPathRef path = CGPathCreateWithRoundedRect(r, radius, radius, nullptr);
    CGContextAddPath(ctx, path);
    CGContextFillPath(ctx);
    CGPathRelease(path);
}

CTFontRef MakeFont(CGFloat size, bool bold) {
    return CTFontCreateUIFontForLanguage(
        bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem,
        size, nullptr);
}

void DrawText(CGContextRef ctx, const std::string& text, CGPoint origin,
              CTFontRef font, RGBA color) {
    if (text.empty()) return;
    NSString* s = [NSString stringWithUTF8String:text.c_str()];
    if (!s) return;
    CGColorRef cg = CGColorCreateGenericRGB(color.r, color.g, color.b, color.a);
    NSDictionary* attrs = @{
        (id)kCTFontAttributeName: (__bridge id)font,
        (id)kCTForegroundColorAttributeName: (__bridge id)cg,
    };
    NSAttributedString* as = [[NSAttributedString alloc] initWithString:s attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
    CGContextSetTextPosition(ctx, origin.x, origin.y);
    CTLineDraw(line, ctx);
    CFRelease(line);
    CGColorRelease(cg);
}

CGFloat MeasureText(const std::string& text, CTFontRef font) {
    if (text.empty()) return 0;
    NSString* s = [NSString stringWithUTF8String:text.c_str()];
    if (!s) return 0;
    NSDictionary* attrs = @{ (id)kCTFontAttributeName: (__bridge id)font };
    NSAttributedString* as = [[NSAttributedString alloc] initWithString:s attributes:attrs];
    CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)as);
    CGRect b = CTLineGetBoundsWithOptions(line, kCTLineBoundsUseOpticalBounds);
    CFRelease(line);
    return b.size.width;
}

} // namespace

struct Ui::Impl {
    CTFontRef bodyFont = nullptr;
    CTFontRef smallFont = nullptr;
    CTFontRef bigFont = nullptr;

    CGImageRef overlayImage = nullptr;

    bool settingsOpen = false;
    bool controlsVisibleFlag = true;
    double lastActivity = 0.0;
    int listenAction = -1;
    int focusedField = -1;

    double CurrentMs() const {
        return (double)[NSDate timeIntervalSinceReferenceDate] * 1000.0;
    }

    ~Impl() {
        if (bodyFont) CFRelease(bodyFont);
        if (smallFont) CFRelease(smallFont);
        if (bigFont) CFRelease(bigFont);
        if (overlayImage) CGImageRelease(overlayImage);
    }
};

void Ui::Initialize(void* /*cgContext*/) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    if (!impl_->bodyFont) impl_->bodyFont = MakeFont(13.0, false);
    if (!impl_->smallFont) impl_->smallFont = MakeFont(11.0, false);
    if (!impl_->bigFont) impl_->bigFont = MakeFont(28.0, true);
    impl_->lastActivity = impl_->CurrentMs();
}

void Ui::Draw(void* cgContextOpaque, const RectF& videoRect, Settings& settings,
              const UiInfo& info) {
    if (!impl_) Initialize(cgContextOpaque);
    CGContextRef ctx = (CGContextRef)cgContextOpaque;
    if (!ctx) return;

    CGContextSaveGState(ctx);

    const CGRect video = CGRectMake(videoRect.left, videoRect.top,
                                    videoRect.right - videoRect.left,
                                    videoRect.bottom - videoRect.top);

    // --- Overlay image (bound to video rect) ---
    if (impl_->overlayImage && !settings.overlayImage.empty()) {
        const CGFloat imgW = CGImageGetWidth(impl_->overlayImage);
        const CGFloat imgH = CGImageGetHeight(impl_->overlayImage);
        const CGFloat scale = settings.overlayScale / 100.0 *
                              std::min(video.size.width, video.size.height) /
                              std::max(imgW, imgH);
        const CGFloat w = imgW * scale, h = imgH * scale;
        const CGFloat margin = 16.0;
        CGFloat x = video.origin.x + margin;
        CGFloat y = video.origin.y + margin;
        switch (settings.overlayPos) {
            case 1: x = CGRectGetMaxX(video) - w - margin; break;                              // TR
            case 2: y = CGRectGetMaxY(video) - h - margin; break;                              // BL
            case 3: x = CGRectGetMaxX(video) - w - margin;
                    y = CGRectGetMaxY(video) - h - margin; break;                              // BR
            case 4: x = video.origin.x + (video.size.width - w) * 0.5;
                    y = video.origin.y + (video.size.height - h) * 0.5; break;                 // Center
            default: break;                                                                     // TL
        }
        CGContextSaveGState(ctx);
        CGContextSetAlpha(ctx, settings.overlayOpacity / 100.0);
        CGContextDrawImage(ctx, CGRectMake(x, y, w, h), impl_->overlayImage);
        CGContextRestoreGState(ctx);
    }

    // --- Crosshair ---
    if (settings.crosshair) {
        const RGBA c = CrossColor(settings.crossColor);
        const CGFloat cx = CGRectGetMidX(video);
        const CGFloat cy = CGRectGetMidY(video);
        const CGFloat sz = (CGFloat)settings.crossSize;
        SetStroke(ctx, c);
        SetFill(ctx, c);
        CGContextSetLineWidth(ctx, 2.0);
        switch (settings.crossStyle) {
            case 0: // + cross with gap
                CGContextBeginPath(ctx);
                CGContextMoveToPoint(ctx, cx - sz, cy); CGContextAddLineToPoint(ctx, cx - 2, cy);
                CGContextMoveToPoint(ctx, cx + 2, cy); CGContextAddLineToPoint(ctx, cx + sz, cy);
                CGContextMoveToPoint(ctx, cx, cy - sz); CGContextAddLineToPoint(ctx, cx, cy - 2);
                CGContextMoveToPoint(ctx, cx, cy + 2); CGContextAddLineToPoint(ctx, cx, cy + sz);
                CGContextStrokePath(ctx);
                break;
            case 1: // dot
                CGContextFillEllipseInRect(ctx, CGRectMake(cx - sz/4, cy - sz/4, sz/2, sz/2));
                break;
            case 2: // circle
                CGContextStrokeEllipseInRect(ctx, CGRectMake(cx - sz, cy - sz, sz*2, sz*2));
                break;
            case 3: // T
                CGContextBeginPath(ctx);
                CGContextMoveToPoint(ctx, cx - sz, cy); CGContextAddLineToPoint(ctx, cx + sz, cy);
                CGContextMoveToPoint(ctx, cx, cy); CGContextAddLineToPoint(ctx, cx, cy + sz);
                CGContextStrokePath(ctx);
                break;
            default: break;
        }
    }

    // --- HUD panel (top-left overlay) ---
    if (settings.hud && info.capturing) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%.0f fps  |  USB %.1f ms  |  App %.1f ms  |  Dropped %llu",
                      info.fps, info.usbMs, info.appMs,
                      (unsigned long long)info.dropped);
        const CGFloat w = MeasureText(buf, impl_->bodyFont) + 20.0;
        const CGRect panel = CGRectMake(video.origin.x + 12.0,
                                        CGRectGetMaxY(video) - 32.0,
                                        w, 22.0);
        FillRoundedRect(ctx, panel, 6.0, PanelBG());
        DrawText(ctx, buf,
                 CGPointMake(panel.origin.x + 10.0, panel.origin.y + 6.0),
                 impl_->bodyFont, kWhite);
    }

    // --- No-signal placeholder ---
    if (!info.hasVideo) {
        std::string msg = info.message.empty() ? std::string("No signal") : info.message;
        const CGFloat w = MeasureText(msg, impl_->bigFont);
        const CGRect r = CGRectMake(CGRectGetMidX(video) - w/2 - 20,
                                    CGRectGetMidY(video) - 28.0,
                                    w + 40.0, 56.0);
        FillRoundedRect(ctx, r, 10.0, PanelBG());
        DrawText(ctx, msg,
                 CGPointMake(r.origin.x + 20.0, r.origin.y + 16.0),
                 impl_->bigFont, kWhite);
    }

    // --- Capture-mode toast ---
    if (info.inputCaptured) {
        std::string msg = "Controller input captured  |  Press Esc to release";
        const CGFloat w = MeasureText(msg, impl_->smallFont) + 24.0;
        const CGRect r = CGRectMake(CGRectGetMidX(video) - w/2,
                                    video.origin.y + 16.0,
                                    w, 22.0);
        FillRoundedRect(ctx, r, 6.0, {Accent().r, Accent().g, Accent().b, 0.72f});
        DrawText(ctx, msg,
                 CGPointMake(r.origin.x + 12.0, r.origin.y + 6.0),
                 impl_->smallFont, kBlack);
    }

    CGContextRestoreGState(ctx);
}

// The Mac UI exposes all commands via the native menu bar (see MacApp.mm), so
// pointer / keyboard hit-testing in-canvas is intentionally a no-op.
bool Ui::MouseMove(float, float) { return false; }
bool Ui::MouseDown(float, float) { return false; }
bool Ui::MouseUp() { return false; }
bool Ui::Wheel(float, float, float) { return false; }
bool Ui::RightDown(float, float) { return false; }

void Ui::Activity() {
    if (!impl_) Initialize(nullptr);
    impl_->lastActivity = impl_->CurrentMs();
}

bool Ui::ControlsVisible() const {
    return impl_ ? impl_->controlsVisibleFlag : true;
}

bool Ui::SettingsOpen() const { return impl_ ? impl_->settingsOpen : false; }

void Ui::ToggleSettings() {
    if (!impl_) Initialize(nullptr);
    impl_->settingsOpen = !impl_->settingsOpen;
    Activity();
}

void Ui::OpenController() {
    if (!impl_) Initialize(nullptr);
    impl_->settingsOpen = true;
    Activity();
}

void Ui::CloseSettings() {
    if (!impl_) Initialize(nullptr);
    impl_->settingsOpen = false;
    impl_->focusedField = -1;
    impl_->listenAction = -1;
}

bool Ui::TextFocused() const { return impl_ ? impl_->focusedField >= 0 : false; }

bool Ui::OnChar(uint32_t) { return false; }
bool Ui::OnTextKey(int, bool) { return false; }

bool Ui::Listening() const { return impl_ ? impl_->listenAction >= 0 : false; }

void Ui::AssignBinding(Settings& /*settings*/, InputCode /*code*/) {
    if (!impl_) return;
    impl_->listenAction = -1;
}

void Ui::CancelListening() {
    if (impl_) impl_->listenAction = -1;
}

std::vector<UiEvent> Ui::TakeEvents() {
    std::vector<UiEvent> out;
    out.swap(events_);
    return out;
}

bool Ui::LoadOverlayImage(void* /*cgContext*/, const std::string& path) {
    if (!impl_) Initialize(nullptr);
    if (impl_->overlayImage) { CGImageRelease(impl_->overlayImage); impl_->overlayImage = nullptr; }
    if (path.empty()) return false;
    NSString* ns = [NSString stringWithUTF8String:path.c_str()];
    NSURL* url = [NSURL fileURLWithPath:ns];
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!src) return false;
    impl_->overlayImage = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    return impl_->overlayImage != nullptr;
}

void Ui::ClearOverlayImage() {
    if (!impl_) return;
    if (impl_->overlayImage) {
        CGImageRelease(impl_->overlayImage);
        impl_->overlayImage = nullptr;
    }
}

Ui::Ui() = default;
Ui::~Ui() {
    if (impl_ && impl_->overlayImage) {
        CGImageRelease(impl_->overlayImage);
        impl_->overlayImage = nullptr;
    }
}
