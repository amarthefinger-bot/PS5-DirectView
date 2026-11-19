#include "MacApp.h"

#include "../Audio.h"
#include "../Capture.h"
#include "../InputManager.h"
#include "../RemotePlayClient.h"
#include "../Renderer.h"
#include "../Ui.h"

#import <Cocoa/Cocoa.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <ApplicationServices/ApplicationServices.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// macOS orchestration layer. Mirrors the Windows App class in main.cpp but
// everything is cooperative with Cocoa's run loop instead of a hand-rolled
// MsgWaitForMultipleObjectsEx loop. Settings are persisted in NSUserDefaults
// under the bundle identifier.

namespace {

constexpr float kSharpLevels[] = {0.0f, 0.3f, 0.6f, 1.0f};

NSString* kDefaults = @"dev.ps5directview";

std::string ToUtf8(NSString* s) { return s ? std::string(s.UTF8String) : std::string(); }
NSString* FromUtf8(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool Contains(const std::string& h, const char* n) { return h.find(n) != std::string::npos; }

int CaptureDeviceScore(const VideoDevice& d) {
    const std::string name = Lower(d.name);
    const std::string link = Lower(d.symbolicLink);
    int score = 0;
    static const char* kCapture[] = {
        "usb video", "capture", "hdmi", "cam link", "elgato", "avermedia",
        "live gamer", "ugreen", "hagibis", "macrosilicon", "ms2109", "ms2130",
        "game", "video grabber", "usb3", "uvc", "4k", "razer ripsaw",
    };
    static const char* kNotCapture[] = {
        "facetime", "ir camera", "infrared", "virtual", "obs",
        "iriun", "camo", "snap camera", "front", "rear",
    };
    for (auto* w : kCapture) if (Contains(name, w)) score += 100;
    for (auto* w : kNotCapture) if (Contains(name, w)) score -= 200;
    return score;
}

// ---------- NSUserDefaults persistence ----------

NSUserDefaults* Defs() { return [NSUserDefaults standardUserDefaults]; }

int LoadInt(NSString* key, int fallback) {
    if (![Defs() objectForKey:key]) return fallback;
    return (int)[Defs() integerForKey:key];
}
void SaveInt(NSString* key, int v) { [Defs() setInteger:v forKey:key]; }
bool LoadBool(NSString* key, bool fallback) {
    if (![Defs() objectForKey:key]) return fallback;
    return [Defs() boolForKey:key];
}
void SaveBool(NSString* key, bool v) { [Defs() setBool:v forKey:key]; }
std::string LoadStr(NSString* key) {
    NSString* s = [Defs() stringForKey:key];
    return s ? ToUtf8(s) : std::string();
}
void SaveStr(NSString* key, const std::string& v) {
    [Defs() setObject:FromUtf8(v) forKey:key];
}

Settings LoadAppSettings() {
    Settings s;
    s.sharpLevel = std::min(LoadInt(@"Sharpening", 2), 3);
    s.rangeMode = std::min(LoadInt(@"Range", 0), 2);
    s.vsync = LoadBool(@"VSync", true);
    s.audioEnabled = LoadBool(@"Audio", true);
    s.audioOutput = LoadStr(@"AudioOutput");
    s.volume = std::clamp(LoadInt(@"Volume", 100), 0, 100);
    s.muted = LoadBool(@"Muted", false);
    s.hud = LoadBool(@"Hud", false);
    s.crosshair = LoadBool(@"Crosshair", false);
    s.crossStyle = std::clamp(LoadInt(@"CrossStyle", 0), 0, 3);
    s.crossColor = std::clamp(LoadInt(@"CrossColor", 0), 0, 5);
    s.crossSize = std::clamp(LoadInt(@"CrossSize", 14), 4, 48);
    s.overlayImage = LoadStr(@"OverlayImage");
    s.overlayPos = std::clamp(LoadInt(@"OverlayPos", 0), 0, 4);
    s.overlayScale = std::clamp(LoadInt(@"OverlayScale", 25), 5, 100);
    s.overlayOpacity = std::clamp(LoadInt(@"OverlayOpacity", 100), 5, 100);
    s.alwaysOnTop = LoadBool(@"AlwaysOnTop", false);
    s.input = LoadInputSettings();
    return s;
}

void SaveAppSettings(const Settings& s) {
    SaveInt(@"Sharpening", s.sharpLevel);
    SaveInt(@"Range", s.rangeMode);
    SaveBool(@"VSync", s.vsync);
    SaveBool(@"Audio", s.audioEnabled);
    SaveStr(@"AudioOutput", s.audioOutput);
    SaveInt(@"Volume", s.volume);
    SaveBool(@"Muted", s.muted);
    SaveBool(@"Hud", s.hud);
    SaveBool(@"Crosshair", s.crosshair);
    SaveInt(@"CrossStyle", s.crossStyle);
    SaveInt(@"CrossColor", s.crossColor);
    SaveInt(@"CrossSize", s.crossSize);
    SaveStr(@"OverlayImage", s.overlayImage);
    SaveInt(@"OverlayPos", s.overlayPos);
    SaveInt(@"OverlayScale", s.overlayScale);
    SaveInt(@"OverlayOpacity", s.overlayOpacity);
    SaveBool(@"AlwaysOnTop", s.alwaysOnTop);
    SaveInputSettings(s.input);
}

} // namespace

// ---------- Overlay NSView (CoreGraphics HUD over the Metal view) ----------

@class MacAppDelegate;

@interface OverlayView : NSView
@property (nonatomic, weak) MacAppDelegate* app;
@end

// ---------- Main window delegate ----------

@interface MacAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, MTKViewDelegate>
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) MTKView* metalView;
@property (nonatomic, strong) OverlayView* overlayView;
@property (nonatomic, strong) NSTimer* statusTimer;
@property (nonatomic, assign) BOOL captured;
@property (nonatomic, assign) BOOL fullscreen;
- (void)buildMenu;
- (void)refreshDevices;
- (void)chooseDeviceAtIndex:(int)index remember:(BOOL)remember;
- (void)onFrameReady;
- (void)applyPicture;
- (void)applyAudioLevels;
- (void)applyAlwaysOnTop;
- (void)toggleCaptureInput;
- (void)statusTick:(NSTimer*)timer;
- (Settings*)settingsPtr;
- (const UiInfo*)buildInfo;
- (Ui*)ui;
@end

// ---------- The App class holds plain C++ state ----------

struct AppState {
    Settings settings;
    UiInfo info;
    std::unique_ptr<CaptureService::FrameEvent> frameEvent;
    std::unique_ptr<CaptureService> capture;
    std::unique_ptr<D3DRenderer> renderer;
    std::unique_ptr<AudioPassthrough> audio;
    std::unique_ptr<InputManager> input;
    std::unique_ptr<RemotePlayClient> remote;
    std::unique_ptr<Ui> ui;

    std::vector<VideoDevice> devices;
    std::vector<AudioOutput> outputs;
    int selectedDevice = -1;

    VideoFrame frame;
    std::uint64_t lastSequence = 0;
    std::uint64_t droppedFrames = 0;
    std::chrono::steady_clock::time_point fpsLast{};
    std::uint64_t fpsLastCount = 0;
    double measuredFps = 0.0;
    double lastReconnectSec = 0.0;

    RpStatus rpStatus;
    std::mutex rpMutex;

    bool capturing() const { return capture && capture->Running(); }
};

static AppState* g_state = nullptr;
static __weak MacAppDelegate* g_delegate = nil;

@implementation OverlayView

- (BOOL)isOpaque { return NO; }
- (BOOL)wantsUpdateLayer { return YES; }
- (void)drawRect:(NSRect)dirtyRect {
    MacAppDelegate* app = self.app;
    if (!app || !g_state) return;
    CGContextRef ctx = NSGraphicsContext.currentContext.CGContext;
    if (!ctx) return;

    const NSRect bounds = self.bounds;
    Ui::RectF video = {
        (float)bounds.origin.x,
        (float)bounds.origin.y,
        (float)(bounds.origin.x + bounds.size.width),
        (float)(bounds.origin.y + bounds.size.height),
    };
    // Pointer-event coordinates: Cocoa origin is bottom-left; CGContext is too.
    [app ui]->Draw((void*)ctx, video, *[app settingsPtr], *[app buildInfo]);
}

- (void)mouseMoved:(NSEvent*)event { [self.app ui]->Activity(); }

@end

// ---------- Delegate impl ----------

@implementation MacAppDelegate {
    AppState* _state;
}

- (instancetype)init {
    if ((self = [super init])) {
        _state = g_state;
    }
    return self;
}

- (Settings*)settingsPtr { return &_state->settings; }
- (Ui*)ui { return _state->ui.get(); }

- (const UiInfo*)buildInfo {
    UiInfo& i = _state->info;
    i.hasVideo = _state->renderer && _state->renderer->HasImage();
    i.capturing = _state->capturing();
    i.testFile = false;
    i.fullscreen = (bool)self.fullscreen;
    i.deviceName = (_state->selectedDevice >= 0 &&
                    _state->selectedDevice < (int)_state->devices.size())
                   ? _state->devices[_state->selectedDevice].name : std::string();
    i.signal = _state->capture ? _state->capture->Signal() : std::string();
    i.message = _state->capture ? _state->capture->Status() : std::string();
    i.fps = _state->measuredFps;
    i.usbMs = 0.0;
    i.appMs = 0.0;
    i.dropped = _state->droppedFrames;
    i.audioStatus = _state->audio ? _state->audio->Status() : std::string();
    i.audioPeak = _state->audio ? _state->audio->Peak() : 0.0f;
    i.devices.clear();
    for (const auto& d : _state->devices) i.devices.push_back(d.name);
    i.selectedDevice = _state->selectedDevice;
    i.outputs = _state->outputs;
    {
        std::lock_guard<std::mutex> lock(_state->rpMutex);
        i.rp = _state->rpStatus;
    }
    i.inputStats = _state->input ? _state->input->Stats() : InputStats{};
    i.pad = _state->input ? _state->input->CurrentPad() : PadState{};
    i.inputCaptured = (bool)self.captured;
    return &i;
}

// ------------ App lifecycle ------------

- (void)applicationDidFinishLaunching:(NSNotification*)note {
    _state->settings = LoadAppSettings();

    [self buildMenu];

    const NSRect frame = NSMakeRect(80, 80, 1300, 780);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:style
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"PS5 DirectView";
    self.window.delegate = self;
    self.window.backgroundColor = [NSColor colorWithCalibratedRed:0.047
                                                            green:0.055
                                                             blue:0.071
                                                            alpha:1.0];
    self.window.acceptsMouseMovedEvents = YES;

    NSView* content = self.window.contentView;
    content.wantsLayer = YES;

    self.metalView = [[MTKView alloc] initWithFrame:content.bounds device:nil];
    self.metalView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:self.metalView];

    self.overlayView = [[OverlayView alloc] initWithFrame:content.bounds];
    self.overlayView.app = self;
    self.overlayView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.overlayView.wantsLayer = YES;
    self.overlayView.layer.backgroundColor = [NSColor clearColor].CGColor;
    [content addSubview:self.overlayView];

    // MetalKit owns drawable sizing, including Retina scale and fullscreen.
    self.metalView.autoResizeDrawable = YES;
    self.metalView.delegate = self;
    self.metalView.preferredFramesPerSecond = 60;

    // Core engine init.
    _state->ui->Initialize(nullptr);
    _state->renderer->Initialize((__bridge void*)self.metalView);
    _state->outputs = ListAudioOutputs();
    [self applyPicture];
    [self applyAudioLevels];
    if (_state->audio) _state->audio->SetOutput(_state->settings.audioOutput);
    if (!_state->settings.overlayImage.empty())
        _state->ui->LoadOverlayImage(nullptr, _state->settings.overlayImage);

    // Remote Play + Input.
    __weak MacAppDelegate* weakSelf = self;
    _state->remote->Start([weakSelf]{
        MacAppDelegate* s = weakSelf;
        if (!s || !g_state) return;
        RpStatus st = g_state->remote->Status();
        std::lock_guard<std::mutex> lock(g_state->rpMutex);
        g_state->rpStatus = st;
    });
    _state->remote->SetAutoReconnect(_state->settings.input.autoReconnect);
    _state->input->SetSettings(_state->settings.input);
    _state->input->Start((__bridge void*)self.window,
        [](const PadState& pad) {
            if (g_state && g_state->remote) g_state->remote->SubmitPad(pad);
        },
        [weakSelf](int /*reason*/) {
            dispatch_async(dispatch_get_main_queue(), ^{
                MacAppDelegate* s = weakSelf;
                if (s) [s setCaptured:NO];
                if (g_state && g_state->input) g_state->input->SetCaptured(false);
            });
        });

    {
        std::lock_guard<std::mutex> lock(g_state->rpMutex);
        g_state->rpStatus = _state->remote->Status();
    }
    if (_state->settings.input.connectOnStartup && _state->rpStatus.paired)
        _state->remote->Connect();

    self.statusTimer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                                        target:self
                                                      selector:@selector(statusTick:)
                                                      userInfo:nil
                                                       repeats:YES];

    [self applyAlwaysOnTop];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    [self refreshDevices];
}

- (void)applicationWillTerminate:(NSNotification*)note {
    SaveAppSettings(_state->settings);
    if (_state->input) _state->input->Stop();
    if (_state->remote) _state->remote->Stop();
    if (_state->capture) _state->capture->Stop();
    if (_state->audio) _state->audio->Stop();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return YES; }

// ------------ MTKViewDelegate ------------

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    // Notification only. Setting drawableSize here re-enters MetalKit's
    // setter before it has finished, causing unbounded recursion on resize.
    // Presentation reads the actual drawable dimensions each frame.
    (void)view;
    (void)size;
}

- (void)drawInMTKView:(MTKView*)view {
    if (!_state->capturing()) {
        // No signal: still clear the swapchain so the window is black.
        _state->renderer->Redraw();
        [self.overlayView setNeedsDisplay:YES];
        return;
    }
    // Pull latest frame (may be the same one we drew last time).
    VideoFrame frame;
    if (_state->capture->TryConsume(frame)) {
        if (_state->lastSequence && frame.sequence > _state->lastSequence + 1)
            _state->droppedFrames += (frame.sequence - _state->lastSequence - 1);
        _state->lastSequence = frame.sequence;
        _state->renderer->Render(frame);
        _state->frame = std::move(frame);

        const auto now = std::chrono::steady_clock::now();
        if (_state->fpsLast.time_since_epoch().count() == 0) _state->fpsLast = now;
        ++_state->fpsLastCount;
        const double elapsed = std::chrono::duration<double>(now - _state->fpsLast).count();
        if (elapsed >= 1.0) {
            _state->measuredFps = _state->fpsLastCount / elapsed;
            _state->fpsLast = now;
            _state->fpsLastCount = 0;
        }
    } else {
        _state->renderer->Redraw();
    }
    [self.overlayView setNeedsDisplay:YES];
}

// ------------ status timer ------------

- (void)statusTick:(NSTimer*)timer {
    // Try reconnect if the capture dropped.
    if (_state->selectedDevice >= 0 && !_state->capturing()) {
        const double now = (double)[NSDate timeIntervalSinceReferenceDate];
        if (now - _state->lastReconnectSec > 3.0) {
            _state->lastReconnectSec = now;
            [self refreshDevices];
        }
    }
    [self.overlayView setNeedsDisplay:YES];
}

// ------------ devices ------------

- (void)refreshDevices {
    std::string previous;
    if (_state->selectedDevice >= 0 &&
        _state->selectedDevice < (int)_state->devices.size()) {
        previous = _state->devices[_state->selectedDevice].symbolicLink;
    }
    if (previous.empty()) previous = LoadStr(@"LastDevice");

    if (_state->capture) _state->capture->Stop();
    _state->renderer->ResetVideo();
    _state->frame = VideoFrame{};
    _state->lastSequence = 0;
    _state->devices = EnumerateVideoDevices();
    _state->selectedDevice = -1;

    [self rebuildDeviceMenu];

    if (_state->devices.empty()) {
        if (_state->audio) _state->audio->Stop();
        return;
    }
    int choice = 0;
    int best = std::numeric_limits<int>::min();
    for (size_t i = 0; i < _state->devices.size(); ++i) {
        int score = CaptureDeviceScore(_state->devices[i]);
        if (!previous.empty() && _state->devices[i].symbolicLink == previous) score += 10000;
        if (score > best) { best = score; choice = (int)i; }
    }
    [self chooseDeviceAtIndex:choice remember:NO];
}

- (void)chooseDeviceAtIndex:(int)index remember:(BOOL)remember {
    if (index < 0 || index >= (int)_state->devices.size()) return;
    _state->capture->Stop();
    _state->renderer->ResetVideo();
    _state->frame = VideoFrame{};
    _state->lastSequence = 0;
    _state->selectedDevice = index;
    const std::string& uid = _state->devices[index].symbolicLink;
    if (remember) SaveStr(@"LastDevice", uid);
    _state->capture->Start(uid);
    if (_state->settings.audioEnabled) _state->audio->Start(uid);
    else _state->audio->Stop();
    [self rebuildDeviceMenu];
}

// ------------ settings application ------------

- (void)applyPicture {
    const int lvl = std::clamp(_state->settings.sharpLevel, 0, 3);
    _state->renderer->SetSharpness(kSharpLevels[lvl]);
    _state->renderer->SetRangeOverride(_state->settings.rangeMode);
    _state->renderer->SetVSync(_state->settings.vsync);
}

- (void)applyAudioLevels {
    _state->audio->SetVolume(_state->settings.volume / 100.0f);
    _state->audio->SetMuted(_state->settings.muted);
}

- (void)applyAlwaysOnTop {
    self.window.level = _state->settings.alwaysOnTop
        ? NSFloatingWindowLevel
        : NSNormalWindowLevel;
}

- (void)toggleCaptureInput {
    self.captured = !self.captured;
    _state->input->SetCaptured(self.captured);
}

// ------------ frame event (unused; MTKViewDelegate drives us) ------------

- (void)onFrameReady { }

// ------------ Menu ------------

- (void)buildMenu {
    NSMenu* mainMenu = [[NSMenu alloc] init];

    // App menu
    NSMenuItem* appItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:appItem];
    NSMenu* appMenu = [[NSMenu alloc] initWithTitle:@"PS5 DirectView"];
    [appMenu addItem:[[NSMenuItem alloc] initWithTitle:@"About PS5 DirectView"
                                                 action:@selector(orderFrontStandardAboutPanel:)
                                          keyEquivalent:@""]];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide" action:@selector(hide:) keyEquivalent:@"h"];
    NSMenuItem* hideOthers = [appMenu addItemWithTitle:@"Hide Others"
                                                 action:@selector(hideOtherApplications:)
                                          keyEquivalent:@"h"];
    hideOthers.keyEquivalentModifierMask = NSEventModifierFlagOption | NSEventModifierFlagCommand;
    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit PS5 DirectView" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    // Capture menu
    NSMenuItem* capItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:capItem];
    NSMenu* capMenu = [[NSMenu alloc] initWithTitle:@"Capture"];
    [capMenu addItemWithTitle:@"Refresh Devices"
                        action:@selector(menuRefreshDevices:)
                 keyEquivalent:@"r"].target = self;
    [capMenu addItem:[NSMenuItem separatorItem]];
    // Device entries appear here; populated by rebuildDeviceMenu.
    capItem.submenu = capMenu;

    // View menu
    NSMenuItem* viewItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:viewItem];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSMenuItem* fs = [viewMenu addItemWithTitle:@"Toggle Fullscreen"
                                          action:@selector(menuToggleFullscreen:)
                                   keyEquivalent:@"f"];
    fs.target = self;
    fs.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    [viewMenu addItem:[NSMenuItem separatorItem]];
    [viewMenu addItemWithTitle:@"Toggle HUD"
                         action:@selector(menuToggleHud:)
                  keyEquivalent:@"i"].target = self;
    [viewMenu addItemWithTitle:@"Toggle Crosshair"
                         action:@selector(menuToggleCrosshair:)
                  keyEquivalent:@""].target = self;
    [viewMenu addItemWithTitle:@"Toggle Always On Top"
                         action:@selector(menuToggleAlwaysOnTop:)
                  keyEquivalent:@""].target = self;
    [viewMenu addItem:[NSMenuItem separatorItem]];
    [viewMenu addItemWithTitle:@"Cycle Sharpness"
                         action:@selector(menuCycleSharp:)
                  keyEquivalent:@"s"].target = self;
    [viewMenu addItemWithTitle:@"Cycle Range Mode"
                         action:@selector(menuCycleRange:)
                  keyEquivalent:@""].target = self;
    [viewMenu addItemWithTitle:@"Motion Interpolation: 30-in-60 (Experimental)"
                         action:@selector(menuToggleFrameGeneration:)
                  keyEquivalent:@"g"].target = self;
    [viewMenu addItemWithTitle:@"FSR 1 Spatial Upscaling (EASU)"
                         action:@selector(menuToggleFsr:)
                  keyEquivalent:@"u"].target = self;
    viewItem.submenu = viewMenu;

    // Audio menu
    NSMenuItem* audItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:audItem];
    NSMenu* audMenu = [[NSMenu alloc] initWithTitle:@"Audio"];
    [audMenu addItemWithTitle:@"Toggle Audio"
                        action:@selector(menuToggleAudio:)
                 keyEquivalent:@""].target = self;
    [audMenu addItemWithTitle:@"Toggle Mute"
                        action:@selector(menuToggleMute:)
                 keyEquivalent:@"m"].target = self;
    [audMenu addItem:[NSMenuItem separatorItem]];
    [audMenu addItemWithTitle:@"Volume Up"
                        action:@selector(menuVolumeUp:)
                 keyEquivalent:@"]"].target = self;
    [audMenu addItemWithTitle:@"Volume Down"
                        action:@selector(menuVolumeDown:)
                 keyEquivalent:@"["].target = self;
    audItem.submenu = audMenu;

    // Remote Play menu
    NSMenuItem* rpItem = [[NSMenuItem alloc] init];
    [mainMenu addItem:rpItem];
    NSMenu* rpMenu = [[NSMenu alloc] initWithTitle:@"Remote Play"];
    [rpMenu addItemWithTitle:@"Discover Consoles"
                       action:@selector(menuRpDiscover:)
                keyEquivalent:@""].target = self;
    [rpMenu addItemWithTitle:@"Pair…"
                       action:@selector(menuRpPair:)
                keyEquivalent:@""].target = self;
    [rpMenu addItemWithTitle:@"Connect"
                       action:@selector(menuRpConnect:)
                keyEquivalent:@""].target = self;
    [rpMenu addItemWithTitle:@"Disconnect"
                       action:@selector(menuRpDisconnect:)
                keyEquivalent:@""].target = self;
    [rpMenu addItem:[NSMenuItem separatorItem]];
    [rpMenu addItemWithTitle:@"Forget Console"
                       action:@selector(menuRpForget:)
                keyEquivalent:@""].target = self;
    [rpMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* cap = [rpMenu addItemWithTitle:@"Capture Input"
                                         action:@selector(menuCaptureInput:)
                                  keyEquivalent:@"c"];
    cap.target = self;
    cap.keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
    rpItem.submenu = rpMenu;

    NSApp.mainMenu = mainMenu;
}

- (void)rebuildDeviceMenu {
    NSMenu* capMenu = NSApp.mainMenu.itemArray[1].submenu;
    while (capMenu.itemArray.count > 2) [capMenu removeItemAtIndex:2];
    if (_state->devices.empty()) {
        NSMenuItem* none = [capMenu addItemWithTitle:@"No capture devices found"
                                               action:nil
                                        keyEquivalent:@""];
        none.enabled = NO;
        return;
    }
    for (size_t i = 0; i < _state->devices.size(); ++i) {
        NSMenuItem* mi = [capMenu addItemWithTitle:FromUtf8(_state->devices[i].name)
                                            action:@selector(menuSelectDevice:)
                                     keyEquivalent:@""];
        mi.target = self;
        mi.tag = (NSInteger)i;
        mi.state = ((int)i == _state->selectedDevice) ? NSControlStateValueOn
                                                       : NSControlStateValueOff;
    }
}

// ------------ Menu actions ------------

- (void)menuRefreshDevices:(id)sender { [self refreshDevices]; }

- (void)menuSelectDevice:(NSMenuItem*)sender {
    [self chooseDeviceAtIndex:(int)sender.tag remember:YES];
}

- (void)menuToggleFullscreen:(id)sender {
    [self.window toggleFullScreen:nil];
    self.fullscreen = !self.fullscreen;
}

- (void)menuToggleHud:(id)sender {
    _state->settings.hud = !_state->settings.hud;
    SaveAppSettings(_state->settings);
}

- (void)menuToggleCrosshair:(id)sender {
    _state->settings.crosshair = !_state->settings.crosshair;
    SaveAppSettings(_state->settings);
}

- (void)menuToggleAlwaysOnTop:(id)sender {
    _state->settings.alwaysOnTop = !_state->settings.alwaysOnTop;
    [self applyAlwaysOnTop];
    SaveAppSettings(_state->settings);
}

- (void)menuToggleFsr:(id)sender {
    const bool enabled = !_state->renderer->FsrUpscaling();
    _state->renderer->SetFsrUpscaling(enabled);
    [(NSMenuItem*)sender setState:enabled ? NSControlStateValueOn : NSControlStateValueOff];
}

- (void)menuToggleFrameGeneration:(id)sender {
    const bool enabled = !_state->renderer->FrameGeneration();
    _state->renderer->SetFrameGeneration(enabled);
    [(NSMenuItem*)sender setState:enabled ? NSControlStateValueOn : NSControlStateValueOff];
}

- (void)menuCycleSharp:(id)sender {
    _state->settings.sharpLevel = (_state->settings.sharpLevel + 1) & 0x3;
    [self applyPicture];
    SaveAppSettings(_state->settings);
}

- (void)menuCycleRange:(id)sender {
    _state->settings.rangeMode = (_state->settings.rangeMode + 1) % 3;
    [self applyPicture];
    SaveAppSettings(_state->settings);
}

- (void)menuToggleAudio:(id)sender {
    _state->settings.audioEnabled = !_state->settings.audioEnabled;
    if (_state->settings.audioEnabled &&
        _state->selectedDevice >= 0 &&
        _state->selectedDevice < (int)_state->devices.size()) {
        _state->audio->Start(_state->devices[_state->selectedDevice].symbolicLink);
    } else {
        _state->audio->Stop();
    }
    SaveAppSettings(_state->settings);
}

- (void)menuToggleMute:(id)sender {
    _state->settings.muted = !_state->settings.muted;
    [self applyAudioLevels];
    SaveAppSettings(_state->settings);
}

- (void)menuVolumeUp:(id)sender {
    _state->settings.volume = std::min(_state->settings.volume + 5, 100);
    [self applyAudioLevels];
    SaveAppSettings(_state->settings);
}

- (void)menuVolumeDown:(id)sender {
    _state->settings.volume = std::max(_state->settings.volume - 5, 0);
    [self applyAudioLevels];
    SaveAppSettings(_state->settings);
}

- (void)menuRpDiscover:(id)sender { _state->remote->Discover(); }

- (void)menuRpConnect:(id)sender { _state->remote->Connect(); }

- (void)menuRpDisconnect:(id)sender { _state->remote->Disconnect(); }

- (void)menuRpForget:(id)sender {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Forget paired console?";
    alert.informativeText = @"You will need to re-pair with a PIN from the PS5.";
    [alert addButtonWithTitle:@"Forget"];
    [alert addButtonWithTitle:@"Cancel"];
    if ([alert runModal] == NSAlertFirstButtonReturn) _state->remote->Forget();
}

- (void)menuRpPair:(id)sender {
    // Simple three-field prompt. Native pair sheets are overkill here.
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Pair PS5";
    alert.informativeText =
        @"Enter the console IP address, the PSN account ID (base64), and the "
        @"PIN shown under Remote Play Connection Settings on the PS5.";
    [alert addButtonWithTitle:@"Pair"];
    [alert addButtonWithTitle:@"Cancel"];

    NSView* v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 110)];
    NSTextField* host = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 78, 320, 24)];
    host.placeholderString = @"192.168.x.y";
    NSTextField* account = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 44, 320, 24)];
    account.placeholderString = @"PSN account ID (base64)";
    NSTextField* pin = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 10, 320, 24)];
    pin.placeholderString = @"PIN (8 digits)";
    [v addSubview:host];
    [v addSubview:account];
    [v addSubview:pin];
    alert.accessoryView = v;

    if ([alert runModal] == NSAlertFirstButtonReturn) {
        _state->remote->Pair(ToUtf8(host.stringValue),
                             ToUtf8(account.stringValue),
                             ToUtf8(pin.stringValue));
    }
}

- (void)menuCaptureInput:(id)sender {
    // Mac requires Accessibility permission for the event tap. Nudge the user.
    if (!AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)@{
        (__bridge id)kAXTrustedCheckOptionPrompt: @YES
    })) {
        NSAlert* a = [[NSAlert alloc] init];
        a.messageText = @"Accessibility permission required";
        a.informativeText =
            @"PS5 DirectView needs Accessibility access to intercept keyboard "
            @"and mouse input for controller emulation. Enable it in "
            @"System Settings > Privacy & Security > Accessibility, then try again.";
        [a runModal];
        return;
    }
    [self toggleCaptureInput];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    SEL s = item.action;
    if (s == @selector(menuToggleHud:))        { item.state = _state->settings.hud; return YES; }
    if (s == @selector(menuToggleCrosshair:))  { item.state = _state->settings.crosshair; return YES; }
    if (s == @selector(menuToggleAlwaysOnTop:)){ item.state = _state->settings.alwaysOnTop; return YES; }
    if (s == @selector(menuToggleAudio:))      { item.state = _state->settings.audioEnabled; return YES; }
    if (s == @selector(menuToggleMute:))       { item.state = _state->settings.muted; return YES; }
    if (s == @selector(menuCaptureInput:))     { item.state = self.captured; return YES; }
    if (s == @selector(menuRpConnect:))        {
        return _state->rpStatus.paired &&
               _state->rpStatus.state != RpState::Connected;
    }
    if (s == @selector(menuRpDisconnect:))     {
        return _state->rpStatus.state == RpState::Connected ||
               _state->rpStatus.state == RpState::Connecting;
    }
    if (s == @selector(menuRpForget:))         { return _state->rpStatus.paired; }
    return YES;
}

// ------------ NSWindowDelegate ------------

- (void)windowWillEnterFullScreen:(NSNotification*)note { self.fullscreen = YES; }
- (void)windowDidExitFullScreen:(NSNotification*)note { self.fullscreen = NO; }

@end

// ---------- Entry point exported to MacEntry.mm ----------

int MacAppRun(int argc, const char* argv[]) {
    @autoreleasepool {
        g_state = new AppState();
        g_state->frameEvent = std::make_unique<CaptureService::FrameEvent>();
        g_state->capture = std::make_unique<CaptureService>(g_state->frameEvent.get());
        g_state->renderer = std::make_unique<D3DRenderer>();
        g_state->audio = std::make_unique<AudioPassthrough>();
        g_state->input = std::make_unique<InputManager>();
        g_state->remote = std::make_unique<RemotePlayClient>();
        g_state->ui = std::make_unique<Ui>();

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        MacAppDelegate* delegate = [[MacAppDelegate alloc] init];
        g_delegate = delegate;
        [NSApp setDelegate:delegate];
        [NSApp run];

        delete g_state;
        g_state = nullptr;
    }
    return 0;
}
