#include "Ui.h"

#ifdef _WIN32
#include "MouseTranslator.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace {
// Palette: neutral charcoal surfaces, one blue accent, status colours.
constexpr UINT32 kBackground = 0x0C0E12;
constexpr UINT32 kPanel = 0x14171D;
constexpr UINT32 kCard = 0x1B1F27;
constexpr UINT32 kCardHover = 0x232833;
constexpr UINT32 kBorder = 0x2A303B;
constexpr UINT32 kText = 0xEDEFF3;
constexpr UINT32 kSubtle = 0x9AA3B2;
constexpr UINT32 kFaint = 0x646C7A;
constexpr UINT32 kAccent = 0x3D8BFF;
constexpr UINT32 kGood = 0x34D399;
constexpr UINT32 kWarn = 0xFBBF24;
constexpr UINT32 kBad = 0xF87171;

constexpr float kTopBar = 60.0f;
constexpr float kBottomBar = 60.0f;
constexpr float kPanelWidth = 384.0f;
constexpr float kPad = 20.0f;
constexpr ULONGLONG kHideAfterMs = 2500;

constexpr UINT32 kCrossColors[] = {0x00FF66, 0xFF3B3B, 0x00E5FF, 0xFFE500, 0xFFFFFF, 0xFF4DFF};

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) glyphs.
constexpr wchar_t kIconSettings = 0xE713;
constexpr wchar_t kIconFullscreen = 0xE740;
constexpr wchar_t kIconWindowed = 0xE73F;
constexpr wchar_t kIconVolume = 0xE767;
constexpr wchar_t kIconMute = 0xE74F;
constexpr wchar_t kIconRefresh = 0xE72C;
constexpr wchar_t kIconVideo = 0xE714;
constexpr wchar_t kIconFile = 0xE8E5;
constexpr wchar_t kIconClose = 0xE8BB;
constexpr wchar_t kIconStats = 0xE9D9;
constexpr wchar_t kIconPicture = 0xE8B9;
constexpr wchar_t kIconDelete = 0xE74D;

D2D1_RECT_F Rect(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
UINT32 RpColor(const RpStatus& rp);

bool Inside(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

std::wstring PickFont(IDWriteFactory* factory, std::initializer_list<const wchar_t*> names) {
    ComPtr<IDWriteFontCollection> fonts;
    factory->GetSystemFontCollection(&fonts);
    for (const wchar_t* name : names) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (fonts && SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists)
            return name;
    }
    return L"Segoe UI";
}
} // namespace

void Ui::Initialize(ID2D1DeviceContext* context) {
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    context->CreateSolidColorBrush(D2D1::ColorF(kText), &brush_);
    const std::wstring text = PickFont(dwrite_.Get(), {L"Segoe UI Variable Text", L"Segoe UI"});
    const std::wstring display = PickFont(dwrite_.Get(), {L"Segoe UI Variable Display", L"Segoe UI"});
    const std::wstring icons = PickFont(dwrite_.Get(), {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});

    auto make = [&](ComPtr<IDWriteTextFormat>& out, const std::wstring& family, float size,
                    DWRITE_FONT_WEIGHT weight, bool wrap) {
        dwrite_->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, size, L"", &out);
        out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            dwrite_->CreateEllipsisTrimmingSign(out.Get(), &ellipsis);
            out->SetTrimming(&trimming, ellipsis.Get());
        }
    };
    make(title_, display, 18.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    make(big_, display, 24.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    make(body_, text, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, false);
    make(bodyBold_, text, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    make(small_, text, 12.5f, DWRITE_FONT_WEIGHT_NORMAL, true);
    make(caption_, text, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    make(icon_, icons, 16.0f, DWRITE_FONT_WEIGHT_NORMAL, false);
    make(iconLarge_, icons, 28.0f, DWRITE_FONT_WEIGHT_NORMAL, false);
    lastActivity_ = GetTickCount64();
}

// ---------------------------------------------------------------- helpers

void Ui::Fill(const D2D1_RECT_F& r, UINT32 rgb, float alpha, float radius) {
    brush_->SetColor(D2D1::ColorF(rgb, alpha * alpha_));
    if (radius > 0) ctx_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
    else ctx_->FillRectangle(r, brush_.Get());
}

void Ui::Stroke(const D2D1_RECT_F& r, UINT32 rgb, float alpha, float radius, float width) {
    brush_->SetColor(D2D1::ColorF(rgb, alpha * alpha_));
    const float h = width * 0.5f;
    const D2D1_RECT_F inset = Rect(r.left + h, r.top + h, r.right - h, r.bottom - h);
    if (radius > 0)
        ctx_->DrawRoundedRectangle(D2D1::RoundedRect(inset, radius, radius), brush_.Get(), width);
    else
        ctx_->DrawRectangle(inset, brush_.Get(), width);
}

void Ui::Text(const std::wstring& text, const D2D1_RECT_F& r, IDWriteTextFormat* format,
              UINT32 rgb, float alpha, DWRITE_TEXT_ALIGNMENT align,
              DWRITE_PARAGRAPH_ALIGNMENT valign) {
    if (text.empty()) return;
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(valign);
    brush_->SetColor(D2D1::ColorF(rgb, alpha * alpha_));
    ctx_->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, r, brush_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Ui::Measure(const std::wstring& text, IDWriteTextFormat* format) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                         format, 4000.0f, 100.0f, &layout))) return 0;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}

void Ui::Icon(wchar_t glyph, const D2D1_RECT_F& r, UINT32 rgb, float alpha,
              IDWriteTextFormat* format) {
    Text(std::wstring(1, glyph), r, format ? format : icon_.Get(), rgb, alpha,
         DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

bool Ui::Hover(const D2D1_RECT_F& r) const {
    return Inside(r, mouseX_, mouseY_) && Inside(clip_, mouseX_, mouseY_);
}

void Ui::AddHit(const D2D1_RECT_F& r, std::function<void()> click,
                std::function<void(float)> drag) {
    hits_.push_back({r, clip_, std::move(click), std::move(drag), nullptr});
}

// ---------------------------------------------------------------- widgets

void Ui::IconButton(const D2D1_RECT_F& r, wchar_t glyph, bool active,
                    std::function<void()> click, const wchar_t* tip) {
    const bool hover = Hover(r);
    if (active) Fill(r, kAccent, 0.22f, 8);
    else if (hover) Fill(r, 0xFFFFFF, 0.08f, 8);
    Icon(glyph, r, active ? kAccent : kText, hover || active ? 1.0f : 0.85f);
    if (hover && tip) {
        const float w = Measure(tip, caption_.Get()) + 16;
        const float cx = (r.left + r.right) / 2;
        D2D1_RECT_F t = Rect(cx - w / 2, r.bottom + 6, cx + w / 2, r.bottom + 28);
        if (t.top > 200) t = Rect(cx - w / 2, r.top - 28, cx + w / 2, r.top - 6);
        Fill(t, 0x000000, 0.85f, 6);
        Text(tip, t, caption_.Get(), kText, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    AddHit(r, std::move(click));
}

void Ui::Button(const D2D1_RECT_F& r, const std::wstring& label, wchar_t glyph,
                std::function<void()> click, bool primary) {
    const bool hover = Hover(r);
    if (primary) Fill(r, kAccent, hover ? 1.0f : 0.9f, 8);
    else {
        Fill(r, hover ? kCardHover : kCard, 1.0f, 8);
        Stroke(r, kBorder, 1.0f, 8);
    }
    float contentW = Measure(label, bodyBold_.Get()) + (glyph ? 24 : 0);
    float x = (r.left + r.right - contentW) / 2;
    if (glyph) {
        Icon(glyph, Rect(x, r.top, x + 18, r.bottom), kText);
        x += 24;
    }
    Text(label, Rect(x, r.top, r.right, r.bottom), bodyBold_.Get(), primary ? 0xFFFFFF : kText);
    AddHit(r, std::move(click));
}

float Ui::SectionTitle(float y, const std::wstring& title) {
    Text(title, Rect(panelX_ + kPad, y, panelX_ + kPanelWidth - kPad, y + 28), caption_.Get(),
         kFaint, 1.0f, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    return y + 36;
}

float Ui::ToggleRow(float y, const std::wstring& label, const std::wstring& hint,
                    bool& value, std::function<void()> changed) {
    const float left = panelX_ + kPad, right = panelX_ + kPanelWidth - kPad;
    const float h = hint.empty() ? 46.0f : 60.0f;
    const D2D1_RECT_F row = Rect(left, y, right, y + h);
    Fill(row, Hover(row) ? kCardHover : kCard, 1.0f, 10);
    const float textTop = hint.empty() ? y : y + 9;
    Text(label, Rect(left + 14, textTop, right - 64, hint.empty() ? y + h : textTop + 22),
         body_.Get(), kText);
    if (!hint.empty())
        Text(hint, Rect(left + 14, textTop + 20, right - 64, y + h - 6), small_.Get(), kFaint,
             1.0f, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    // Switch.
    const float cy = y + h / 2;
    const D2D1_RECT_F track = Rect(right - 54, cy - 11, right - 14, cy + 11);
    if (value) Fill(track, kAccent, 1.0f, 11);
    else { Fill(track, 0x0E1014, 1.0f, 11); Stroke(track, 0x4A5260, 1.0f, 11, 1.5f); }
    const float kx = value ? track.right - 11 : track.left + 11;
    brush_->SetColor(D2D1::ColorF(value ? 0xFFFFFF : 0xB8C0CC, alpha_));
    ctx_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), 7.5f, 7.5f), brush_.Get());
    bool* target = &value;
    AddHit(row, [target, changed] { *target = !*target; if (changed) changed(); });
    return y + h + 8;
}

float Ui::Segmented(float y, const std::wstring& label, const std::vector<std::wstring>& options,
                    int& value, std::function<void()> changed) {
    const float left = panelX_ + kPad, right = panelX_ + kPanelWidth - kPad;
    float top = y;
    if (!label.empty()) {
        Text(label, Rect(left, y, right, y + 20), body_.Get(), kSubtle);
        top = y + 26;
    }
    const D2D1_RECT_F bar = Rect(left, top, right, top + 36);
    Fill(bar, kCard, 1.0f, 9);
    const float w = (bar.right - bar.left - 6) / options.size();
    int* target = &value;
    for (size_t i = 0; i < options.size(); ++i) {
        const D2D1_RECT_F seg = Rect(bar.left + 3 + w * i, bar.top + 3,
                                     bar.left + 3 + w * (i + 1), bar.bottom - 3);
        const bool selected = value == static_cast<int>(i);
        if (selected) Fill(seg, kAccent, 1.0f, 7);
        else if (Hover(seg)) Fill(seg, 0xFFFFFF, 0.06f, 7);
        Text(options[i], seg, selected ? bodyBold_.Get() : body_.Get(),
             selected ? 0xFFFFFF : kSubtle, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        const int index = static_cast<int>(i);
        AddHit(seg, [target, index, changed] {
            if (*target != index) { *target = index; if (changed) changed(); }
        });
    }
    return bar.bottom + 16;
}

void Ui::Slider(const D2D1_RECT_F& track, int& value, int minimum, int maximum,
                std::function<void()> changed) {
    const float cy = (track.top + track.bottom) / 2;
    const float t = maximum > minimum ?
        static_cast<float>(value - minimum) / static_cast<float>(maximum - minimum) : 0.0f;
    const float x = track.left + t * (track.right - track.left);
    Fill(Rect(track.left, cy - 2, track.right, cy + 2), 0x3A414D, 1.0f, 2);
    Fill(Rect(track.left, cy - 2, x, cy + 2), kAccent, 1.0f, 2);
    const D2D1_RECT_F hitRect = Rect(track.left - 8, cy - 14, track.right + 8, cy + 14);
    const float r = Hover(hitRect) ? 8.5f : 7.5f;
    brush_->SetColor(D2D1::ColorF(0xFFFFFF, alpha_));
    ctx_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), r, r), brush_.Get());
    int* target = &value;
    const float left = track.left, width = track.right - track.left;
    AddHit(hitRect, nullptr, [target, minimum, maximum, left, width, changed](float mx) {
        const float k = std::clamp((mx - left) / width, 0.0f, 1.0f);
        const int v = minimum + static_cast<int>(std::lround(k * (maximum - minimum)));
        if (v != *target) { *target = v; if (changed) changed(); }
    });
}

float Ui::SliderRow(float y, const std::wstring& label, int& value, int minimum, int maximum,
                    const wchar_t* unit, std::function<void()> changed) {
    const float left = panelX_ + kPad, right = panelX_ + kPanelWidth - kPad;
    Text(label, Rect(left, y, right - 60, y + 20), body_.Get(), kSubtle);
    Text(std::to_wstring(value) + unit, Rect(right - 80, y, right, y + 20), bodyBold_.Get(),
         kText, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
    Slider(Rect(left + 8, y + 26, right - 8, y + 46), value, minimum, maximum, std::move(changed));
    return y + 58;
}

float Ui::RadioRow(float y, const std::wstring& label, bool selected,
                   std::function<void()> click) {
    const float left = panelX_ + kPad, right = panelX_ + kPanelWidth - kPad;
    const D2D1_RECT_F row = Rect(left, y, right, y + 40);
    if (selected) Fill(row, kAccent, 0.14f, 9);
    else if (Hover(row)) Fill(row, kCardHover, 1.0f, 9);
    const D2D1_POINT_2F c = D2D1::Point2F(left + 20, y + 20);
    brush_->SetColor(D2D1::ColorF(selected ? kAccent : 0x5A6272, alpha_));
    ctx_->DrawEllipse(D2D1::Ellipse(c, 7.5f, 7.5f), brush_.Get(), 1.6f);
    if (selected) ctx_->FillEllipse(D2D1::Ellipse(c, 4.0f, 4.0f), brush_.Get());
    Text(label, Rect(left + 38, y, right - 10, y + 40), body_.Get(), selected ? kText : kSubtle);
    AddHit(row, std::move(click));
    return y + 42;
}

// ---------------------------------------------------------------- screens

void Ui::DrawOverlays(const D2D1_RECT_F& video, const Settings& s, const UiInfo& info) {
    alpha_ = 1.0f;
    const float vw = video.right - video.left, vh = video.bottom - video.top;

    // Image overlay (stream frame, watermark, map...).
    if (overlayBitmap_ && !s.overlayImage.empty()) {
        const D2D1_SIZE_F size = overlayBitmap_->GetSize();
        D2D1_RECT_F dest = video;
        if (s.overlayPos != 0 && size.width > 0) {
            const float w = vw * s.overlayScale / 100.0f;
            const float h = w * size.height / size.width;
            const float m = 16.0f;
            const bool rightSide = s.overlayPos == 2 || s.overlayPos == 4;
            const bool bottom = s.overlayPos == 3 || s.overlayPos == 4;
            const float x = rightSide ? video.right - m - w : video.left + m;
            const float y = bottom ? video.bottom - m - h : video.top + m;
            dest = Rect(x, y, x + w, y + h);
        }
        ctx_->DrawBitmap(overlayBitmap_.Get(), dest, s.overlayOpacity / 100.0f,
                         D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // Crosshair, centred on the game image (outlined so it reads on any scene).
    if (s.crosshair) {
        const UINT32 color = kCrossColors[std::clamp(s.crossColor, 0, 5)];
        const D2D1_POINT_2F c = D2D1::Point2F(std::floor((video.left + video.right) / 2) + 0.5f,
                                              std::floor((video.top + video.bottom) / 2) + 0.5f);
        const float L = static_cast<float>(s.crossSize);
        auto lines = [&](float width, UINT32 rgb, float a) {
            brush_->SetColor(D2D1::ColorF(rgb, a));
            const float gap = s.crossStyle == 1 ? std::max(3.0f, L * 0.35f) : 0.0f;
            switch (s.crossStyle) {
                case 0: case 1:
                    ctx_->DrawLine(D2D1::Point2F(c.x - L, c.y), D2D1::Point2F(c.x - gap, c.y), brush_.Get(), width);
                    ctx_->DrawLine(D2D1::Point2F(c.x + gap, c.y), D2D1::Point2F(c.x + L, c.y), brush_.Get(), width);
                    ctx_->DrawLine(D2D1::Point2F(c.x, c.y - L), D2D1::Point2F(c.x, c.y - gap), brush_.Get(), width);
                    ctx_->DrawLine(D2D1::Point2F(c.x, c.y + gap), D2D1::Point2F(c.x, c.y + L), brush_.Get(), width);
                    break;
                case 2:
                    ctx_->FillEllipse(D2D1::Ellipse(c, std::max(2.0f, L / 5) + width / 2 - 1,
                                                    std::max(2.0f, L / 5) + width / 2 - 1), brush_.Get());
                    break;
                default:
                    ctx_->DrawEllipse(D2D1::Ellipse(c, L, L), brush_.Get(), width);
                    ctx_->FillEllipse(D2D1::Ellipse(c, width, width), brush_.Get());
                    break;
            }
        };
        lines(4.0f, 0x000000, 0.55f);
        lines(2.0f, color, 1.0f);
    }

    // Performance HUD.
    if (s.hud && info.hasVideo) {
        const float top = video.top + 12 + (fade_ > 0.01f ? kTopBar * fade_ : 0);
        const D2D1_RECT_F box = Rect(video.left + 12, top, video.left + 236, top + 74);
        Fill(box, 0x000000, 0.62f, 10);
        wchar_t line[128];
        swprintf_s(line, L"%.0f FPS", info.fps);
        Text(line, Rect(box.left + 12, box.top + 8, box.right - 12, box.top + 28), bodyBold_.Get(),
             info.fps >= 55 ? kGood : info.fps >= 25 ? kWarn : kBad);
        if (info.usbMs > 0)
            swprintf_s(line, L"Latency %.1f ms  (USB %.1f + app %.2f)",
                       info.usbMs + info.appMs, info.usbMs, info.appMs);
        else
            swprintf_s(line, L"App latency %.2f ms", info.appMs);
        Text(line, Rect(box.left + 12, box.top + 28, box.right - 8, box.top + 46), caption_.Get(), kText);
        swprintf_s(line, L"Dropped %llu", info.dropped);
        Text(line, Rect(box.left + 12, box.top + 46, box.left + 100, box.top + 64), caption_.Get(), kSubtle);
        const D2D1_RECT_F meter = Rect(box.left + 104, box.top + 53, box.right - 12, box.top + 57);
        Fill(meter, 0x3A414D, 1.0f, 2);
        const float p = std::sqrt(std::clamp(info.audioPeak, 0.0f, 1.0f));
        Fill(Rect(meter.left, meter.top, meter.left + (meter.right - meter.left) * p, meter.bottom),
             p > 0.95f ? kBad : p > 0.75f ? kWarn : kGood, 1.0f, 2);

        // Controller section: only while mouse & keyboard is captured or the PS5 link is up.
        if (info.inputCaptured || info.rp.state == RpState::Connected) {
            const D2D1_RECT_F pad = Rect(box.left, box.bottom + 8, box.right, box.bottom + 96);
            Fill(pad, 0x000000, 0.62f, 10);
            swprintf_s(line, L"PS5 %s%s", RpStateName(info.rp.state), info.inputCaptured ? L" \x00B7 captured" : L"");
            Text(line, Rect(pad.left + 12, pad.top + 6, pad.right - 70, pad.top + 24), caption_.Get(),
                 RpColor(info.rp));
            swprintf_s(line, L"Mouse %.0f Hz \x00B7 %.0f upd/s", info.inputStats.mouseHz,
                       info.inputStats.padUpdatesPerSec);
            Text(line, Rect(pad.left + 12, pad.top + 24, pad.right - 70, pad.top + 42), caption_.Get(), kText);
            swprintf_s(line, L"Input %.0f / %.0f \x00B5s", info.inputStats.processAvgUs,
                       info.inputStats.processMaxUs);
            Text(line, Rect(pad.left + 12, pad.top + 42, pad.right - 70, pad.top + 60), caption_.Get(), kSubtle);
            if (info.rp.state == RpState::Connected) {
                swprintf_s(line, L"Send %.0f \x00B5s \x00B7 RTT %.1f ms", info.rp.submitAvgUs, info.rp.rttMs);
                Text(line, Rect(pad.left + 12, pad.top + 60, pad.right - 70, pad.top + 78), caption_.Get(), kSubtle);
            }
            // Mini sticks.
            auto mini = [&](float cx, float cy, float x, float y) {
                brush_->SetColor(D2D1::ColorF(0x3A414D, 1.0f));
                ctx_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 14, 14), brush_.Get(), 1.2f);
                brush_->SetColor(D2D1::ColorF(kAccent, 1.0f));
                ctx_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx + x * 14, cy + y * 14), 3.5f, 3.5f), brush_.Get());
            };
            mini(pad.right - 50, pad.top + 26, info.pad.lx / 32767.0f, info.pad.ly / 32767.0f);
            mini(pad.right - 20, pad.top + 26, info.pad.rx / 32767.0f, info.pad.ry / 32767.0f);
            int held = 0;
            for (int i = 0; i < kPadButtonCount; ++i) held += info.pad.Pressed(static_cast<PadButton>(i)) ? 1 : 0;
            held += info.pad.l2 ? 1 : 0;
            held += info.pad.r2 ? 1 : 0;
            swprintf_s(line, L"%d held", held);
            Text(line, Rect(pad.right - 66, pad.top + 48, pad.right - 6, pad.top + 66), caption_.Get(), kFaint,
                 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
}

void Ui::DrawNoSignal(const D2D1_SIZE_F& size, const UiInfo& info) {
    alpha_ = 1.0f;
    Fill(Rect(0, 0, size.width, size.height), kBackground);
    const float cx = size.width / 2, cy = size.height / 2;
    const D2D1_RECT_F card = Rect(cx - 250, cy - 150, cx + 250, cy + 150);
    Fill(card, kPanel, 1.0f, 16);
    Stroke(card, kBorder, 1.0f, 16);

    const bool connecting = info.capturing;
    const D2D1_RECT_F badge = Rect(cx - 30, card.top + 30, cx + 30, card.top + 90);
    Fill(badge, connecting ? kAccent : kWarn, 0.16f, 30);
    Icon(kIconVideo, badge, connecting ? kAccent : kWarn, 1.0f, iconLarge_.Get());
    Text(connecting ? L"Waiting for signal" : L"No capture device",
         Rect(card.left, card.top + 100, card.right, card.top + 134), big_.Get(), kText, 1.0f,
         DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(info.message, Rect(card.left + 36, card.top + 138, card.right - 36, card.top + 214),
         small_.Get(), kSubtle, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER,
         DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    Button(Rect(cx - 166, card.bottom - 66, cx - 6, card.bottom - 26), L"Settings",
           kIconSettings, [this] { settingsOpen_ = true; }, true);
    Button(Rect(cx + 6, card.bottom - 66, cx + 166, card.bottom - 26), L"Refresh", kIconRefresh,
           [this] { events_.push_back({UiCommand::RefreshDevices}); });
}

void Ui::DrawTopBar(const D2D1_SIZE_F& size, Settings& s, const UiInfo& info) {
    alpha_ = fade_;
    Fill(Rect(0, 0, size.width, kTopBar), 0x07080B, 0.78f);
    Fill(Rect(0, kTopBar - 1, size.width, kTopBar), 0xFFFFFF, 0.06f);

    // Brand mark + device.
    const D2D1_RECT_F mark = Rect(16, 14, 48, 46);
    Fill(mark, kAccent, 1.0f, 9);
    Icon(kIconVideo, mark, 0xFFFFFF);
    Text(L"DirectView", Rect(60, 10, 260, 30), bodyBold_.Get(), kText);
    const std::wstring device = info.testFile ? L"Test video" :
        info.deviceName.empty() ? L"No device" : info.deviceName;
    Text(device, Rect(60, 30, 260, 48), caption_.Get(), kFaint);

    // Signal chip.
    float x = 60 + std::max(Measure(L"DirectView", bodyBold_.Get()),
                            std::min(200.0f, Measure(device, caption_.Get()))) + 20;
    const std::wstring signal = info.hasVideo ? info.signal :
        info.capturing ? L"Waiting for signal" : L"Disconnected";
    const float chipW = Measure(signal, body_.Get()) + 34;
    const D2D1_RECT_F chip = Rect(x, 16, x + chipW, 44);
    Fill(chip, kCard, 1.0f, 14);
    brush_->SetColor(D2D1::ColorF(info.hasVideo ? kGood : info.capturing ? kWarn : kBad, alpha_));
    ctx_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(chip.left + 14, 30), 4, 4), brush_.Get());
    Text(signal, Rect(chip.left + 24, chip.top, chip.right - 8, chip.bottom), body_.Get(), kText);

    // Right cluster: latency, overlays, settings, fullscreen.
    float r = size.width - 16;
    IconButton(Rect(r - 40, 10, r, 50), info.fullscreen ? kIconWindowed : kIconFullscreen, false,
               [this] { events_.push_back({UiCommand::ToggleFullscreen}); },
               info.fullscreen ? L"Exit fullscreen (F11)" : L"Fullscreen (F11)");
    r -= 46;
    IconButton(Rect(r - 40, 10, r, 50), kIconSettings, settingsOpen_,
               [this] { settingsOpen_ = !settingsOpen_; }, L"Settings (Tab)");
    r -= 46;
    {
        const D2D1_RECT_F pad = Rect(r - 40, 10, r, 50);
        IconButton(pad, 0xE7FC, info.inputCaptured, [this] { OpenController(); }, L"Controller (P)");
        brush_->SetColor(D2D1::ColorF(RpColor(info.rp), alpha_));
        ctx_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(pad.right - 9, pad.top + 9), 3.5f, 3.5f), brush_.Get());
    }
    r -= 46;
    IconButton(Rect(r - 40, 10, r, 50), kIconStats, s.hud,
               [this, &s] { s.hud = !s.hud; events_.push_back({UiCommand::OverlaysChanged}); },
               L"Performance overlay (H)");
    r -= 56;
    if (info.hasVideo && info.appMs > 0) {
        wchar_t value[32];
        swprintf_s(value, L"%.1f ms", info.usbMs + info.appMs);
        const float w = std::max(Measure(value, bodyBold_.Get()), 50.0f) + 4;
        Text(L"LATENCY", Rect(r - w, 12, r, 28), caption_.Get(), kFaint, 1.0f,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        Text(value, Rect(r - w - 20, 26, r, 48), bodyBold_.Get(), kGood, 1.0f,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
}

void Ui::DrawBottomBar(const D2D1_SIZE_F& size, Settings& s, const UiInfo& info) {
    alpha_ = fade_;
    const float top = size.height - kBottomBar;
    Fill(Rect(0, top, size.width, size.height), 0x07080B, 0.78f);
    Fill(Rect(0, top, size.width, top + 1), 0xFFFFFF, 0.06f);
    const float cy = top + kBottomBar / 2;

    const bool silent = s.muted || !s.audioEnabled;
    IconButton(Rect(16, cy - 20, 56, cy + 20), silent ? kIconMute : kIconVolume, false,
               [this, &s] { s.muted = !s.muted; events_.push_back({UiCommand::VolumeChanged}); },
               s.muted ? L"Unmute (M)" : L"Mute (M)");
    Slider(Rect(70, cy - 10, 200, cy + 10), s.volume, 0, 100,
           [this] { events_.push_back({UiCommand::VolumeChanged}); });
    Text(std::to_wstring(s.volume) + L"%", Rect(212, cy - 10, 254, cy + 10), caption_.Get(),
         kSubtle);

    // Input level meter.
    const D2D1_RECT_F meter = Rect(262, cy - 3, 382, cy + 3);
    Fill(meter, 0x2C323C, 1.0f, 3);
    if (s.audioEnabled) {
        const float p = std::sqrt(std::clamp(info.audioPeak, 0.0f, 1.0f));
        Fill(Rect(meter.left, meter.top, meter.left + (meter.right - meter.left) * p, meter.bottom),
             p > 0.95f ? kBad : p > 0.75f ? kWarn : kGood, 1.0f, 3);
    }
    Text(s.audioEnabled ? info.audioStatus : L"Audio passthrough off",
         Rect(398, cy - 10, size.width - 120, cy + 10), caption_.Get(), kFaint);

    IconButton(Rect(size.width - 102, cy - 20, size.width - 62, cy + 20), kIconFile, false,
               [this] { events_.push_back({UiCommand::OpenTestVideo}); }, L"Open test video");
    IconButton(Rect(size.width - 56, cy - 20, size.width - 16, cy + 20), kIconRefresh, false,
               [this] { events_.push_back({UiCommand::RefreshDevices}); }, L"Refresh devices (F5)");
}

float Ui::DrawVideoTab(float y, Settings& s, const UiInfo& info) {
    const float left = panelX_ + kPad, right = panelX_ + kPanelWidth - kPad;
    auto emit = [this](UiCommand c, int i = 0) { return [this, c, i] { events_.push_back({c, i}); }; };

    // Input.
    y = SectionTitle(y, L"INPUT");
    if (info.devices.empty()) {
        Text(L"No capture devices found", Rect(left, y, right, y + 30), body_.Get(), kFaint);
        y += 36;
    }
    for (size_t i = 0; i < info.devices.size(); ++i)
        y = RadioRow(y, info.devices[i], static_cast<int>(i) == info.selectedDevice && !info.testFile,
                     emit(UiCommand::SelectDevice, static_cast<int>(i)));
    const float half = (right - left - 8) / 2;
    Button(Rect(left, y + 4, left + half, y + 40), L"Refresh", kIconRefresh,
           emit(UiCommand::RefreshDevices));
    Button(Rect(left + half + 8, y + 4, right, y + 40), L"Test video", kIconFile,
           emit(UiCommand::OpenTestVideo));
    y += 52;

    // Capture modes (available native device modes are selected by Media Foundation).
    y = SectionTitle(y, L"CAPTURE MODE");
    y = Segmented(y, L"Resolution", {L"Auto", L"1080p", L"1440p", L"2160p"},
                  s.capture.resolution, emit(UiCommand::CaptureModeChanged));
    y = Segmented(y, L"Frame rate", {L"Auto", L"30", L"60", L"120"},
                  s.capture.fps, emit(UiCommand::CaptureModeChanged));
    y = Segmented(y, L"Pixel format (decoded output)", {L"Auto", L"NV12", L"YUY2"},
                  s.capture.output, emit(UiCommand::CaptureModeChanged));
    y = Paragraph(y, L"Switching restarts video capture. Unsupported combinations "
                         L"fall back to Auto; the top-left signal shows the actual mode.",
                  kSubtle, 50);

    // Picture.
    y = SectionTitle(y, L"PICTURE");
    y = Segmented(y, L"Sharpening", {L"Off", L"Low", L"Medium", L"High"}, s.sharpLevel,
                  emit(UiCommand::PictureChanged));
    y = Segmented(y, L"Color range", {L"Auto", L"Full", L"Limited"}, s.rangeMode,
                  emit(UiCommand::PictureChanged));
    y = ToggleRow(y, L"V-Sync", L"No tearing, but adds up to 1 refresh of delay", s.vsync,
                  emit(UiCommand::PictureChanged));

    // Audio.
    y = ToggleRow(y, L"FSR 1 spatial upscaling (F6)", L"EASU, active when the image is enlarged", s.fsrUpscaling,
                  emit(UiCommand::PictureChanged));
    y = ToggleRow(y, L"30 fps motion interpolation (F7)", L"Experimental: 30 fps inside 60 Hz only; adds delay", s.frameGeneration,
                  emit(UiCommand::PictureChanged));
    y = SectionTitle(y, L"AUDIO");
    y = ToggleRow(y, L"HDMI audio passthrough", L"Play the capture card's audio on this PC",
                  s.audioEnabled, emit(UiCommand::AudioToggled));
    if (s.audioEnabled) {
        Text(L"Output device", Rect(left, y, right, y + 20), body_.Get(), kSubtle);
        y += 26;
        y = RadioRow(y, L"Windows default", s.audioOutput.empty(), [this, &s] {
            s.audioOutput.clear();
            events_.push_back({UiCommand::AudioOutputChanged});
        });
        for (const auto& output : info.outputs) {
            const std::wstring id = output.id;
            y = RadioRow(y, output.name, s.audioOutput == id, [this, &s, id] {
                s.audioOutput = id;
                events_.push_back({UiCommand::AudioOutputChanged});
            });
        }
        y += 8;
        y = SliderRow(y, L"Volume", s.volume, 0, 100, L"%", emit(UiCommand::VolumeChanged));
    }

    // Overlays.
    y = SectionTitle(y, L"OVERLAYS");
    y = ToggleRow(y, L"Performance overlay", L"FPS, latency and dropped frames", s.hud,
                  emit(UiCommand::OverlaysChanged));
    y = ToggleRow(y, L"Crosshair", L"Drawn at the centre of the game image", s.crosshair,
                  emit(UiCommand::OverlaysChanged));
    if (s.crosshair) {
        y = Segmented(y, L"Style", {L"Cross", L"Gap", L"Dot", L"Circle"}, s.crossStyle,
                      emit(UiCommand::OverlaysChanged));
        Text(L"Color", Rect(left, y, right, y + 20), body_.Get(), kSubtle);
        y += 28;
        for (int i = 0; i < 6; ++i) {
            const float cx = left + 16 + i * 42.0f;
            const D2D1_RECT_F swatch = Rect(cx - 15, y, cx + 15, y + 30);
            brush_->SetColor(D2D1::ColorF(kCrossColors[i], 1.0f));
            ctx_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, y + 15), 11, 11), brush_.Get());
            if (s.crossColor == i || Hover(swatch)) {
                brush_->SetColor(D2D1::ColorF(s.crossColor == i ? 0xFFFFFF : kSubtle, 1.0f));
                ctx_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, y + 15), 14.5f, 14.5f),
                                  brush_.Get(), 2.0f);
            }
            AddHit(swatch, [this, &s, i] {
                s.crossColor = i;
                events_.push_back({UiCommand::OverlaysChanged});
            });
        }
        y += 42;
        y = SliderRow(y, L"Size", s.crossSize, 4, 48, L" px", emit(UiCommand::OverlaysChanged));
    }

    Text(L"Image overlay", Rect(left, y, right, y + 20), body_.Get(), kSubtle);
    y += 26;
    if (!s.overlayImage.empty()) {
        const size_t slash = s.overlayImage.find_last_of(L"\\/");
        const std::wstring name = slash == std::wstring::npos ? s.overlayImage :
                                  s.overlayImage.substr(slash + 1);
        const D2D1_RECT_F file = Rect(left, y, right, y + 40);
        Fill(file, kCard, 1.0f, 9);
        Icon(kIconPicture, Rect(left + 8, y, left + 36, y + 40), kSubtle);
        Text(name, Rect(left + 40, y, right - 48, y + 40), body_.Get(), kText);
        IconButton(Rect(right - 40, y + 4, right - 4, y + 36), kIconDelete, false,
                   emit(UiCommand::ClearOverlayImage), nullptr);
        y += 48;
        y = Segmented(y, L"Position", {L"Fill", L"\x2196", L"\x2197", L"\x2199", L"\x2198"},
                      s.overlayPos, emit(UiCommand::OverlaysChanged));
        if (s.overlayPos != 0)
            y = SliderRow(y, L"Size", s.overlayScale, 5, 100, L"%", emit(UiCommand::OverlaysChanged));
        y = SliderRow(y, L"Opacity", s.overlayOpacity, 5, 100, L"%", emit(UiCommand::OverlaysChanged));
    } else {
        Button(Rect(left, y, right, y + 40), L"Choose image (PNG, JPG)...", kIconPicture,
               emit(UiCommand::ChooseOverlayImage));
        y += 52;
    }

    // Window.
    y = SectionTitle(y, L"WINDOW");
    y = ToggleRow(y, L"Always on top", L"Keep the game above other windows", s.alwaysOnTop,
                  emit(UiCommand::AlwaysOnTopChanged));
    bool fullscreen = info.fullscreen;
    y = ToggleRow(y, L"Fullscreen", L"Lowest latency: frames go straight to the display",
                  fullscreen, emit(UiCommand::ToggleFullscreen));

    y = SectionTitle(y, L"SHORTCUTS");
    Text(L"F11  Fullscreen     Tab  Settings     M  Mute\n"
         L"H  Performance overlay     C  Crosshair\n"
         L"S  Sharpening     R  Color range     F5  Refresh\n"
         L"F8  Capture mouse & keyboard     P  Controller",
         Rect(left, y, right, y + 84), small_.Get(), kFaint, 1.0f,
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    y += 100;

    return y;
}

void Ui::DrawSettings(const D2D1_SIZE_F& size, Settings& s, const UiInfo& info) {
    alpha_ = 1.0f;
    panelX_ = size.width - kPanelWidth;
    const D2D1_RECT_F panel = Rect(panelX_, 0, size.width, size.height);
    Fill(panel, kPanel, 1.0f);
    Fill(Rect(panelX_, 0, panelX_ + 1, size.height), kBorder);
    AddHit(panel, [] {}); // swallow clicks on empty panel space

    Text(L"Settings", Rect(panelX_ + kPad, 14, size.width - 60, 46), title_.Get(), kText);
    IconButton(Rect(size.width - 52, 12, size.width - 16, 48), kIconClose, false,
               [this] { settingsOpen_ = false; }, nullptr);

    const float viewTop = 60;
    viewHeight_ = size.height - viewTop;
    clip_ = Rect(panelX_, viewTop, size.width, size.height);
    ctx_->PushAxisAlignedClip(clip_, D2D1_ANTIALIAS_MODE_ALIASED);
    float y = viewTop + 4 - scroll_;
    const float left = panelX_ + kPad, right = size.width - kPad;

    // Tabs.
    {
        const D2D1_RECT_F bar = Rect(left, y, right, y + 36);
        Fill(bar, kCard, 1.0f, 9);
        const wchar_t* names[] = {L"Video & audio", L"Controller"};
        const float w = (bar.right - bar.left - 6) / 2;
        for (int i = 0; i < 2; ++i) {
            const D2D1_RECT_F seg = Rect(bar.left + 3 + w * i, bar.top + 3,
                                         bar.left + 3 + w * (i + 1), bar.bottom - 3);
            const bool selected = tab_ == i;
            if (selected) Fill(seg, kAccent, 1.0f, 7);
            else if (Hover(seg)) Fill(seg, 0xFFFFFF, 0.06f, 7);
            Text(names[i], seg, selected ? bodyBold_.Get() : body_.Get(),
                 selected ? 0xFFFFFF : kSubtle, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
            AddHit(seg, [this, i] {
                if (tab_ != i) { tab_ = i; scroll_ = 0; focusedField_ = -1; listenAction_ = -1; }
            });
        }
        y += 48;
    }
    y = tab_ == 0 ? DrawVideoTab(y, s, info) : DrawControllerTab(y, s, info);

    ctx_->PopAxisAlignedClip();
    contentHeight_ = y + scroll_ - viewTop;
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentHeight_ - viewHeight_));

    // Scrollbar.
    if (contentHeight_ > viewHeight_) {
        const float trackH = viewHeight_ - 16;
        const float thumbH = std::max(40.0f, trackH * viewHeight_ / contentHeight_);
        const float thumbY = viewTop + 8 + (trackH - thumbH) * scroll_ /
                             (contentHeight_ - viewHeight_);
        Fill(Rect(size.width - 6, thumbY, size.width - 3, thumbY + thumbH), 0xFFFFFF, 0.18f, 1.5f);
    }
    clip_ = Rect(0, 0, size.width, size.height);
}

void Ui::Draw(ID2D1DeviceContext* context, const D2D1_RECT_F& video, Settings& settings,
              const UiInfo& info) {
    ctx_ = context;
    hits_.clear();
    const D2D1_SIZE_F size = context->GetSize();
    clip_ = Rect(0, 0, size.width, size.height);
    ctx_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // Bars fade out after a few seconds without mouse activity, but stay
    // while settings are open, nothing is playing, or the pointer is on them.
    const bool onBars = mouseY_ >= 0 && (mouseY_ < kTopBar || mouseY_ > size.height - kBottomBar);
    const ULONGLONG idle = GetTickCount64() - lastActivity_;
    if (info.inputCaptured && !settingsOpen_)
        fade_ = 0.0f; // playing: nothing over the game except the capture hint
    else if (settingsOpen_ || !info.hasVideo || activeDrag_ || (onBars && idle < 30000) ||
        idle < kHideAfterMs)
        fade_ = 1.0f;
    else
        fade_ = std::max(0.0f, 1.0f - static_cast<float>(idle - kHideAfterMs) / 300.0f);

    if (info.hasVideo) DrawOverlays(video, settings, info);
    else DrawNoSignal(size, info);
    if (fade_ > 0.01f) {
        DrawTopBar(size, settings, info);
        DrawBottomBar(size, settings, info);
    }
    if (settingsOpen_) DrawSettings(size, settings, info);
    DrawCaptureToast(size, info);
    alpha_ = 1.0f;
}

// ---------------------------------------------------------------- input

bool Ui::MouseMove(float x, float y) {
    mouseX_ = x;
    mouseY_ = y;
    Activity();
    if (activeDrag_) activeDrag_(x);
    return true;
}

bool Ui::MouseDown(float x, float y) {
    mouseX_ = x;
    mouseY_ = y;
    Activity();
    focusedField_ = -1; // a text field click re-focuses below
    listenAction_ = -1;
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!Inside(it->rect, x, y) || !Inside(it->clip, x, y)) continue;
        if (it->drag) { activeDrag_ = it->drag; activeDrag_(x); }
        if (it->click) it->click();
        return true;
    }
    if (settingsOpen_ && x < panelX_) { settingsOpen_ = false; return true; }
    return false;
}

bool Ui::MouseUp() {
    const bool had = static_cast<bool>(activeDrag_);
    activeDrag_ = nullptr;
    return had;
}

bool Ui::Wheel(float x, float, float delta) {
    Activity();
    if (!settingsOpen_ || x < panelX_) return false;
    scroll_ = std::clamp(scroll_ - delta / 120.0f * 56.0f, 0.0f,
                         std::max(0.0f, contentHeight_ - viewHeight_));
    return true;
}

std::vector<UiEvent> Ui::TakeEvents() {
    std::vector<UiEvent> out;
    out.swap(events_);
    return out;
}

bool Ui::LoadOverlayImage(ID2D1DeviceContext* context, const std::wstring& path) {
    overlayBitmap_.Reset();
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnLoad, &decoder))) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeMedianCut))) return false;
    return SUCCEEDED(context->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &overlayBitmap_));
}

// ---------------------------------------------------------------- controller

namespace {
constexpr wchar_t kIconGamepad = 0xE7FC;
constexpr wchar_t kIconMouse = 0xE962;
constexpr wchar_t kIconWarning = 0xE7BA;
constexpr wchar_t kIconSearch = 0xE721;
constexpr wchar_t kIconLink = 0xE71B;

UINT32 RpColor(const RpStatus& rp) {
    switch (rp.state) {
        case RpState::Connected: return kGood;
        case RpState::Pairing: case RpState::Searching: case RpState::Waking:
        case RpState::Connecting: case RpState::Reconnecting: return kWarn;
        default: return rp.error ? kBad : kFaint;
    }
}
} // namespace

float Ui::Paragraph(float y, const std::wstring& text, UINT32 rgb, float minHeight) {
    const float left = panelX_ + kPad, right = panelX_ + kPanelWidth - kPad;
    float height = minHeight;
    ComPtr<IDWriteTextLayout> layout;
    if (!text.empty() &&
        SUCCEEDED(dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                            small_.Get(), right - left, 2000.0f, &layout))) {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        height = std::max(height, m.height + 2);
    }
    Text(text, Rect(left, y, right, y + height), small_.Get(), rgb, 1.0f,
         DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return y + height + 8;
}

float Ui::TextField(float y, const std::wstring& label, std::wstring& value, int id,
                    const wchar_t* placeholder) {
    const float left = panelX_ + kPad, right = panelX_ + kPanelWidth - kPad;
    Text(label, Rect(left, y, right, y + 20), body_.Get(), kSubtle);
    const D2D1_RECT_F box = Rect(left, y + 24, right, y + 62);
    const bool focused = focusedField_ == id;
    Fill(box, 0x0E1014, 1.0f, 8);
    Stroke(box, focused ? kAccent : (Hover(box) ? 0x4A5260 : kBorder), 1.0f, 8, focused ? 1.5f : 1.0f);
    const D2D1_RECT_F inner = Rect(box.left + 12, box.top, box.right - 12, box.bottom);
    if (value.empty() && !focused)
        Text(placeholder, inner, body_.Get(), kFaint);
    else
        Text(value, inner, body_.Get(), kText);
    if (focused) {
        const float x = std::min(inner.left + Measure(value, body_.Get()) + 1, inner.right);
        Fill(Rect(x, box.top + 10, x + 1.5f, box.bottom - 10), kAccent);
    }
    AddHit(box, [this, id] { focusedField_ = id; });
    return y + 72;
}

void Ui::StickView(const D2D1_RECT_F& r, float x, float y, const wchar_t* label) {
    const D2D1_POINT_2F c = D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2 - 8);
    const float radius = std::min(r.right - r.left, r.bottom - r.top - 20) / 2 - 2;
    brush_->SetColor(D2D1::ColorF(0x0E1014, alpha_));
    ctx_->FillEllipse(D2D1::Ellipse(c, radius, radius), brush_.Get());
    brush_->SetColor(D2D1::ColorF(kBorder, alpha_));
    ctx_->DrawEllipse(D2D1::Ellipse(c, radius, radius), brush_.Get(), 1.2f);
    ctx_->DrawLine(D2D1::Point2F(c.x - radius, c.y), D2D1::Point2F(c.x + radius, c.y), brush_.Get(), 1.0f);
    ctx_->DrawLine(D2D1::Point2F(c.x, c.y - radius), D2D1::Point2F(c.x, c.y + radius), brush_.Get(), 1.0f);
    const D2D1_POINT_2F dot = D2D1::Point2F(c.x + x * radius, c.y + y * radius);
    brush_->SetColor(D2D1::ColorF(kAccent, alpha_));
    ctx_->DrawLine(c, dot, brush_.Get(), 2.0f);
    ctx_->FillEllipse(D2D1::Ellipse(dot, 5, 5), brush_.Get());
    Text(label, Rect(r.left, r.bottom - 18, r.right, r.bottom), caption_.Get(), kFaint, 1.0f,
         DWRITE_TEXT_ALIGNMENT_CENTER);
}

float Ui::DrawControllerTab(float y, Settings& s, const UiInfo& info) {
    const float left = panelX_ + kPad, right = panelX_ + kPanelWidth - kPad;
    auto emit = [this](UiCommand c, int i = 0) { return [this, c, i] { events_.push_back({c, i}); }; };
    const auto changed = emit(UiCommand::InputChanged);
    InputSettings& in = s.input;
    const RpStatus& rp = info.rp;

    // Fair-play notice.
    {
        const std::wstring notice =
            L"Mouse & keyboard emulation may break the rules of some online games. "
            L"Aim assist is applied by the game; driving an analog stick with a mouse "
            L"cannot match native mouse precision. No macros, recoil scripts or automated aiming.";
        ComPtr<IDWriteTextLayout> layout;
        float h = 60;
        if (SUCCEEDED(dwrite_->CreateTextLayout(notice.c_str(), static_cast<UINT32>(notice.size()),
                                                small_.Get(), right - left - 52, 400, &layout))) {
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            h = m.height + 20;
        }
        const D2D1_RECT_F card = Rect(left, y, right, y + h);
        Fill(card, kWarn, 0.10f, 10);
        Icon(kIconWarning, Rect(left + 8, y + 8, left + 36, y + 32), kWarn);
        Text(notice, Rect(left + 42, y + 10, right - 10, y + h - 6), small_.Get(), kSubtle, 1.0f,
             DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        y += h + 10;
    }

    // ------------------------------------------------ PS5 connection
    y = SectionTitle(y, L"PS5 REMOTE PLAY (INPUT ONLY)");
    {
        const D2D1_RECT_F card = Rect(left, y, right, y + 58);
        Fill(card, kCard, 1.0f, 10);
        brush_->SetColor(D2D1::ColorF(RpColor(rp), alpha_));
        ctx_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + 18, y + 20), 5, 5), brush_.Get());
        std::wstring title = RpStateName(rp.state);
        if (rp.state == RpState::Connected && rp.rttMs > 0) {
            wchar_t t[48];
            swprintf_s(t, L"  \x00B7  RTT %.1f ms", rp.rttMs);
            title += t;
        }
        Text(title, Rect(left + 32, y + 8, right - 12, y + 32), bodyBold_.Get(), kText);
        const std::wstring console =
            rp.paired ? rp.consoleName + L"  \x00B7  " + rp.host : std::wstring(L"No PS5 paired yet");
        Text(console, Rect(left + 32, y + 30, right - 12, y + 50), caption_.Get(), kFaint);
        y += 66;
    }
    if (!rp.message.empty()) y = Paragraph(y, rp.message, rp.error ? kBad : kSubtle, 18);

    const float half = (right - left - 8) / 2;
    if (rp.paired) {
        const bool busy = rp.state != RpState::Disconnected && rp.state != RpState::NotPaired;
        if (busy)
            Button(Rect(left, y, left + half, y + 38), L"Disconnect", kIconLink,
                   emit(UiCommand::RpDisconnect));
        else
            Button(Rect(left, y, left + half, y + 38), L"Connect", kIconLink,
                   emit(UiCommand::RpConnect), true);
        Button(Rect(left + half + 8, y, right, y + 38), showPairForm_ ? L"Hide pairing" : L"Pair again",
               0, [this] { showPairForm_ = !showPairForm_; });
        y += 50;
    }
    if (!rp.paired || showPairForm_) {
        y = Paragraph(y, L"One-time pairing: on the PS5 open Settings \x203A System \x203A Remote Play, "
                         L"turn on Enable Remote Play, choose Pair Device and enter the 8-digit code "
                         L"below with your PSN account ID. The pairing keys are stored encrypted "
                         L"(Windows DPAPI) for your Windows account only.", kSubtle, 18);
        Button(Rect(left, y, right, y + 38),
               rp.discovering ? L"Searching\x2026" : L"Find PS5 on this network", kIconSearch,
               emit(UiCommand::RpDiscover));
        y += 48;
        for (const FoundConsole& c : rp.found) {
            const std::wstring label =
                c.name + L"  \x00B7  " + c.host + (c.ready ? L"" : L"  (rest mode)");
            const std::wstring host = c.host;
            y = RadioRow(y, label, pair_.host == host, [this, host] { pair_.host = host; });
        }
        y = TextField(y, L"PS5 IP address", pair_.host, 0, L"e.g. 192.168.1.50");
        y = TextField(y, L"PSN account ID", pair_.account, 1,
                      L"12-character code ending in = (or decimal ID)");
        y = TextField(y, L"PIN from the PS5", pair_.pin, 2, L"8 digits");
        Button(Rect(left, y, right, y + 40), L"Pair with PS5", kIconLink, emit(UiCommand::RpPair), true);
        y += 50;
        y = Paragraph(y, L"Your account ID is not your sign-in name. Find it with chiaki-ng's "
                         L"\"PSN account ID\" helper or a PSN account-ID lookup, and pair with the PSN "
                         L"account you play with on the PS5.", kFaint, 18);
    }
    if (rp.paired) {
        Button(Rect(left, y, right, y + 36), L"Forget this PS5", kIconDelete, emit(UiCommand::RpForget));
        y += 46;
    }
    y = ToggleRow(y, L"Reconnect automatically", L"Retry after drops, PS5 restarts and rest mode",
                  in.autoReconnect, changed);
    y = ToggleRow(y, L"Connect when DirectView starts", L"", in.connectOnStartup, changed);

    // ------------------------------------------------ mouse & keyboard
    y = SectionTitle(y, L"MOUSE & KEYBOARD");
    y = ToggleRow(y, L"Mouse & keyboard controller", L"Raw mouse input drives the right stick",
                  in.enabled, changed);
    Button(Rect(left, y, right, y + 42),
           info.inputCaptured ? L"Captured \x2014 press F8 or Esc to release"
                              : L"Capture mouse & keyboard (F8)",
           kIconMouse, emit(UiCommand::CaptureInput), in.enabled);
    y += 54;

    AimSettings& a = in.aim;
    y = SliderRow(y, L"Sensitivity", a.sensitivity, 1, 200, L"", changed);
    y = SliderRow(y, L"Vertical sensitivity", a.verticalPct, 25, 300, L"%", changed);
    y = Segmented(y, L"Response curve", {L"Linear", L"Power"}, a.curve, changed);
    if (a.curve == 1)
        y = SliderRow(y, L"Curve exponent (x100, 100 = linear)", a.exponentPct, 50, 300, L"", changed);
    y = SliderRow(y, L"Anti-deadzone (skip game's stick deadzone)", a.antiDeadzonePct, 0, 50, L"%", changed);
    y = SliderRow(y, L"Deadzone (ignore tiny movements)", a.deadzonePct, 0, 30, L"%", changed);
    y = SliderRow(y, L"Acceleration", a.accelPct, 0, 200, L"%", changed);
    y = SliderRow(y, L"Smoothing (0 = none)", a.smoothingMs, 0, 50, L" ms", changed);
    y = SliderRow(y, L"Speed window", a.windowMs, 2, 32, L" ms", changed);
    y = SliderRow(y, L"Recenter after no movement", a.recenterMs, 2, 100, L" ms", changed);
    y = SliderRow(y, L"Walk key stick scale", in.walkPct, 10, 100, L"%", changed);
    y = ToggleRow(y, L"Invert vertical aim", L"", a.invertY, changed);

    // Response preview: mouse speed (x) -> stick deflection (y).
    {
        const D2D1_RECT_F g = Rect(left, y, right, y + 110);
        Fill(g, 0x0E1014, 1.0f, 8);
        const float px = g.left + 10, py = g.bottom - 10;
        const float w = g.right - g.left - 20, h = g.bottom - g.top - 20;
        brush_->SetColor(D2D1::ColorF(kBorder, alpha_));
        ctx_->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(px + w, py), brush_.Get(), 1.0f);
        ctx_->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(px, py - h), brush_.Get(), 1.0f);
        brush_->SetColor(D2D1::ColorF(kAccent, alpha_));
        D2D1_POINT_2F prev = D2D1::Point2F(px, py);
        for (int i = 1; i <= 64; ++i) {
            const float m = i / 64.0f;
            const float d = MouseTranslator::Shape(m, a);
            const D2D1_POINT_2F p = D2D1::Point2F(px + m * w, py - d * h);
            ctx_->DrawLine(prev, p, brush_.Get(), 2.0f);
            prev = p;
        }
        Text(L"mouse speed \x2192", Rect(g.right - 140, g.bottom - 26, g.right - 10, g.bottom - 8),
             caption_.Get(), kFaint, 1.0f, DWRITE_TEXT_ALIGNMENT_TRAILING);
        Text(L"stick", Rect(px + 6, g.top + 6, px + 80, g.top + 22), caption_.Get(), kFaint);
        y += 120;
    }

    // ------------------------------------------------ mapping
    y = SectionTitle(y, L"CONTROLLER MAPPING");
    y = Paragraph(y, L"Click a slot, then press a key or mouse button. Right-click a slot to clear "
                     L"it. F8 and Esc always release capture and can't be bound.", kFaint, 18);
    const float slotW = 104;
    for (int i = 0; i < kActionCount; ++i) {
        const D2D1_RECT_F row = Rect(left, y, right, y + 34);
        if (Hover(row)) Fill(row, kCardHover, 0.6f, 6);
        Text(ActionName(static_cast<Action>(i)), Rect(left + 8, y, right - 2 * slotW - 16, y + 34),
             body_.Get(), kText);
        for (int slot = 0; slot < kSlotsPerAction; ++slot) {
            const float x0 = right - (kSlotsPerAction - slot) * (slotW + 6);
            const D2D1_RECT_F r = Rect(x0, y + 3, x0 + slotW, y + 31);
            const bool listening = listenAction_ == i && listenSlot_ == slot;
            const InputCode code = in.bindings[static_cast<size_t>(i)][static_cast<size_t>(slot)];
            Fill(r, listening ? kAccent : (Hover(r) ? kCardHover : kCard), listening ? 0.3f : 1.0f, 6);
            Stroke(r, listening ? kAccent : kBorder, 1.0f, 6);
            Text(listening ? std::wstring(L"Press a key\x2026") : InputCodeName(code),
                 Rect(r.left + 6, r.top, r.right - 6, r.bottom), caption_.Get(),
                 code || listening ? kText : kFaint, 1.0f, DWRITE_TEXT_ALIGNMENT_CENTER);
            InputSettings* settings = &in;
            hits_.push_back({r, clip_,
                             [this, i, slot] { listenAction_ = i; listenSlot_ = slot; focusedField_ = -1; },
                             nullptr,
                             [this, settings, i, slot, changed] {
                                 settings->bindings[static_cast<size_t>(i)][static_cast<size_t>(slot)] =
                                     input_code::kNone;
                                 listenAction_ = -1;
                                 changed();
                             }});
        }
        y += 36;
    }
    {
        InputSettings* settings = &in;
        Button(Rect(left, y + 6, right, y + 42), L"Reset mapping to defaults", kIconRefresh,
               [this, settings, changed] {
                   settings->bindings = DefaultBindings();
                   listenAction_ = -1;
                   changed();
               });
    }
    y += 56;

    // ------------------------------------------------ diagnostics
    y = SectionTitle(y, L"LIVE INPUT & TIMING");
    {
        const float w = (right - left - 8) / 2;
        StickView(Rect(left, y, left + w, y + 130), info.pad.lx / 32767.0f, info.pad.ly / 32767.0f,
                  L"Left stick");
        StickView(Rect(left + w + 8, y, right, y + 130), info.pad.rx / 32767.0f,
                  info.pad.ry / 32767.0f, L"Right stick");
        y += 138;
        std::wstring pressed;
        auto add = [&](const wchar_t* name) { pressed += (pressed.empty() ? L"" : L", "); pressed += name; };
        for (int i = 0; i < kPadButtonCount; ++i)
            if (info.pad.Pressed(static_cast<PadButton>(i))) add(PadButtonName(static_cast<PadButton>(i)));
        if (info.pad.l2) add(L"L2");
        if (info.pad.r2) add(L"R2");
        y = Paragraph(y, L"Buttons: " + (pressed.empty() ? std::wstring(L"none") : pressed), kSubtle, 18);
    }
    {
        const InputStats& st = info.inputStats;
        wchar_t rtt[32] = L"\x2014";
        if (rp.state == RpState::Connected && rp.rttMs > 0) swprintf_s(rtt, L"%.1f ms", rp.rttMs);
        wchar_t text[768];
        swprintf_s(text,
                   L"Mouse reports: %.0f Hz (while captured)\n"
                   L"Input processing (WM_INPUT \x2192 state handed off): avg %.1f \x00B5s, max %.1f \x00B5s\n"
                   L"Input timer lateness: avg %.0f \x00B5s\n"
                   L"Controller state changes: %.0f /s\n"
                   L"Hand-off into Remote Play sender: avg %.1f \x00B5s, max %.1f \x00B5s\n"
                   L"Remote Play sender: stick state sent at most every 8 ms (125 Hz)\n"
                   L"Network round trip (Remote Play test): %s\n"
                   L"Video: HDMI capture path unchanged; Remote Play A/V dropped undecoded",
                   st.mouseHz, st.processAvgUs, st.processMaxUs, st.tickJitterUs, st.padUpdatesPerSec,
                   rp.submitAvgUs, rp.submitMaxUs, rtt);
        y = Paragraph(y, text, kSubtle, 18);
    }
    return y + 16;
}

void Ui::DrawCaptureToast(const D2D1_SIZE_F& size, const UiInfo& info) {
    if (info.inputCaptured && !wasCaptured_) capturedTick_ = GetTickCount64();
    wasCaptured_ = info.inputCaptured;
    if (!info.inputCaptured) return;
    const ULONGLONG age = GetTickCount64() - capturedTick_;
    if (age > 3000) return;
    alpha_ = age < 2600 ? 1.0f : 1.0f - static_cast<float>(age - 2600) / 400.0f;
    const std::wstring text = info.rp.state == RpState::Connected
        ? L"Mouse & keyboard \x2192 PS5   \x00B7   F8 or Esc to release"
        : L"Mouse & keyboard captured (PS5 not connected)   \x00B7   F8 or Esc to release";
    const float w = Measure(text, bodyBold_.Get()) + 56;
    const D2D1_RECT_F r = Rect(size.width / 2 - w / 2, 24, size.width / 2 + w / 2, 64);
    Fill(r, 0x000000, 0.78f, 20);
    Icon(kIconGamepad, Rect(r.left + 12, r.top, r.left + 40, r.bottom), RpColor(info.rp));
    Text(text, Rect(r.left + 44, r.top, r.right - 12, r.bottom), bodyBold_.Get(), kText);
    alpha_ = 1.0f;
}

bool Ui::RightDown(float x, float y) {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!Inside(it->rect, x, y) || !Inside(it->clip, x, y)) continue;
        if (it->rightClick) { it->rightClick(); return true; }
        return false;
    }
    return false;
}

bool Ui::OnChar(wchar_t ch) {
    if (!TextFocused()) return false;
    std::wstring& value = focusedField_ == 0 ? pair_.host : focusedField_ == 1 ? pair_.account : pair_.pin;
    if (ch < 0x20 || ch == 0x7F) return true; // control characters: see OnTextKey
    if (focusedField_ == 2 && (ch < L'0' || ch > L'9')) return true;
    const size_t limit = focusedField_ == 2 ? 8 : 64;
    if (value.size() < limit) value += ch;
    return true;
}

bool Ui::OnTextKey(WPARAM key, bool ctrl) {
    if (!TextFocused()) return false;
    std::wstring& value = focusedField_ == 0 ? pair_.host : focusedField_ == 1 ? pair_.account : pair_.pin;
    switch (key) {
        case VK_BACK: if (!value.empty()) value.pop_back(); break;
        case VK_DELETE: value.clear(); break;
        case VK_ESCAPE: focusedField_ = -1; break;
        case VK_TAB: focusedField_ = (focusedField_ + 1) % 3; break;
        case VK_RETURN:
            if (focusedField_ == 2) { focusedField_ = -1; events_.push_back({UiCommand::RpPair}); }
            else ++focusedField_;
            break;
        case 'V':
            if (ctrl && OpenClipboard(nullptr)) {
                if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
                    if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                        for (const wchar_t* c = text; *c; ++c)
                            if (*c > 0x20) OnChar(*c); // skip whitespace/newlines from pastes
                        GlobalUnlock(data);
                    }
                }
                CloseClipboard();
            }
            break;
        default: break;
    }
    return true;
}

void Ui::AssignBinding(Settings& settings, InputCode code) {
    if (listenAction_ < 0 || listenAction_ >= kActionCount) return;
    settings.input.bindings[static_cast<size_t>(listenAction_)][static_cast<size_t>(listenSlot_)] = code;
    listenAction_ = -1;
    events_.push_back({UiCommand::InputChanged});
}

#endif // _WIN32
