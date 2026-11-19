# PS5 DirectView — macOS build (preview)

> **Status: UNVERIFIED.** This Mac port compiles in theory but has never
> actually been built or run on real hardware yet. Expect compile errors on
> first run. The CI workflow `.github/workflows/build-macos.yml` is the first
> real integration test — check its output.

macOS 12 Monterey or later. Universal (arm64 + x86_64). Compiled with the
Apple clang that ships with Xcode 14+.

---

## 1. Install build tools

```sh
# Xcode command-line tools (compiler, SDK, codesign)
xcode-select --install

# Homebrew: https://brew.sh
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# CMake + Ninja
brew install cmake ninja
```

## 2. Build

From the repo root:

```sh
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"

cmake --build build --parallel
```

Artifact: `build/PS5DirectView.app` — a standard macOS application bundle.

## 3. First-run permissions (IMPORTANT)

Unsigned apps built locally are quarantined and sandboxed by macOS. You will
need to grant three permissions by hand.

### 3a. Clear the quarantine flag

The first time you launch the `.app`, macOS will refuse ("cannot be opened,
developer cannot be verified"). Strip the quarantine attribute:

```sh
xattr -dr com.apple.quarantine build/PS5DirectView.app
```

If that fails or you need to make the inner binary executable:

```sh
chmod +x build/PS5DirectView.app/Contents/MacOS/PS5DirectView
```

### 3b. Camera permission (for the HDMI capture card)

macOS classifies USB capture cards as cameras. On first use, you'll see
a system prompt — click **Allow**. If you miss it:

- **System Settings → Privacy & Security → Camera → PS5 DirectView** → enable.

### 3c. Microphone permission (for HDMI audio)

Same deal: the audio arrives on the capture card's CoreAudio input, which the
system reports as a microphone.

- **System Settings → Privacy & Security → Microphone → PS5 DirectView** → enable.

### 3d. Accessibility permission (required for controller emulation)

Controller emulation needs a global event tap to steal mouse and keyboard
input. macOS requires Accessibility permission for that.

- **System Settings → Privacy & Security → Accessibility → PS5 DirectView** →
  enable. You can toggle the checkbox after adding the bundle.

Without this, the **Capture Input** menu item will show a prompt and then
do nothing.

## 4. Running

```sh
open build/PS5DirectView.app
```

Or launch it from Finder. All commands live in the **menu bar**, not on
the window:

| Menu | Items |
|------|-------|
| Capture | Refresh devices · list of detected capture cards |
| View | Fullscreen (⌃⌘F) · HUD (⌘I) · Crosshair · Always On Top · Sharpness (⌘S) · Range |
| Audio | Toggle · Mute (⌘M) · Volume ± (⌘] / ⌘[) |
| Remote Play | Discover · Pair… · Connect · Disconnect · Forget · Capture Input (⌃⌘C) |

Press **Esc** at any time to release captured input.

## 5. Remote Play pairing

1. On the PS5: **Settings → System → Remote Play → Link Device**.
2. Note the 8-digit PIN and your PSN account ID (base64) — the account ID
   is the same one the Windows app asks for. See the main `README.md`
   for how to look it up.
3. In the app: **Remote Play → Pair…**, fill in host IP, account ID, PIN.

Pairing state is stored in the macOS Keychain under service
`dev.ps5directview`.

## 6. Developer notes

### Where the Mac-specific code lives

```
src/mac/
    MacEntry.mm           int main() -> MacAppRun
    MacApp.h / MacApp.mm  NSApplicationDelegate + menu + MTKView host
    Capture_Mac.mm        AVCaptureSession on CMIO DAL devices
    Renderer_Mac.mm       Metal (MTKView + CVMetalTextureCache)
    Audio_Mac.mm          AVCaptureAudioDataOutput -> AUHAL default output
    InputManager_Mac.mm   CGEventTap on a dedicated CFRunLoop thread
    Ui_Mac.mm             CoreGraphics overlay (HUD/crosshair/no-signal)
    SecureStore_Mac.mm    Keychain (SecItem*)
    Info.plist.in         bundle plist with usage descriptions
```

### Rebuilding after changes

```sh
cmake --build build --parallel
```

Clean rebuild:

```sh
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

### Tests

```sh
cmake --build build --target InputTests CryptoTests
./build/InputTests
./build/CryptoTests
```

### Debugging in Xcode

```sh
cmake -S . -B build-xcode -G Xcode -DCMAKE_OSX_ARCHITECTURES=arm64
open build-xcode/PS5DirectView.xcodeproj
```

### Signing (optional, for distribution)

```sh
codesign --force --deep --sign - build/PS5DirectView.app
```

Replace `-` with a Developer ID identity for distribution; the plain `-`
dash does an ad-hoc signature that only works on the local machine.

## 7. Known limitations vs. Windows build

- **No in-canvas settings panel.** Windows renders an immediate-mode Direct2D
  settings UI inside the video window. On macOS all commands live in the
  menu bar instead — more native, but less discoverable.
- **No DWMA-style frame tearing.** Metal's presentation model does not expose
  a direct equivalent to DXGI `DXGI_SWAP_EFFECT_FLIP_DISCARD` with
  `ALLOW_TEARING`. VSync is effectively always on.
- **No MMCSS-equivalent thread scheduling.** QoS classes help but aren't the
  same as the Windows "Games" MMCSS task.
- **Frame generation / FSR / DLSS: not implemented.** Explicit non-goals.

## 8. If the build fails

Most likely causes, in order:

1. Objective-C ARC + C++20 interop quirks in `.mm` files.
2. `CVMetalTextureCacheCreateTextureFromImage` API-level mismatch on the
   configured deployment target.
3. `AudioUnit` deprecated-API warnings becoming errors under stricter flags.
4. CoreMediaIO DAL enablement needing `CMIOObjectSetPropertyData` on
   `kCMIOHardwarePropertyAllowScreenCaptureDevices` before device enumeration
   (already handled, but may change in future macOS releases).

Report issues at https://github.com/babymonie/PS5-DirectView/issues with the
full CMake and compiler output.
