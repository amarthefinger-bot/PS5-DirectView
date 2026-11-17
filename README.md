# PS5 DirectView

[![Build](https://github.com/babymonie/PS5-DirectView/actions/workflows/build-windows.yml/badge.svg)](https://github.com/babymonie/PS5-DirectView/actions/workflows/build-windows.yml)

A small Windows app for playing your PS5 on a PC monitor through a cheap USB HDMI capture card, with as little delay as I could get out of it. It can also turn your mouse and keyboard into a PS5 controller, so you can watch the capture card and play with MnK at the same time.

It's one exe, around 1 MB. No OBS, no Python, no extra hardware.

## What it does

**Video**
- Shows the capture card full screen (or windowed) using Direct3D 11.
- Always draws the newest frame. There's no frame queue in the app, and V-Sync is off by default.
- NV12 / YUY2 are converted on the GPU.
- Optional audio passthrough from the capture card to any output device.
- Picture options: sharpening, colour range (auto / full / limited), V-Sync.
- Overlays: FPS and latency stats, a crosshair, and an image overlay (PNG/JPG). They're drawn into the same frame as the video, so they add no delay.
- Reconnects on its own if the card is unplugged or the PS5 restarts.

**Mouse and keyboard to PS5**
- Connects to the PS5 over Remote Play on your local network, but only to send controller input. The video still comes from the HDMI card. The PS5 stream is thrown away before it's decrypted or decoded.
- The PS5 sees a normal controller, so games use their normal controller aim assist.
- Mouse drives the right stick, WASD the left stick, and every button can be rebound (two keys per action).
- Aim tuning: sensitivity, vertical sensitivity, deadzone, anti-deadzone, linear or power curve, acceleration, smoothing (can be zero), speed window, recenter time and invert Y.
- F8 grabs the mouse and keyboard; F8 or Esc lets go. Switching windows lets go too, and every held button is released.
- Pairing is done once. The keys are stored encrypted with Windows DPAPI.
- If the connection drops, it reconnects by itself. It can also wake the PS5 from rest mode.
- Shows live sticks and measured timings, so you can see what is actually happening instead of guessing.

## Measured on my setup

Ryzen 7 9800X3D, RTX 5080, 280 Hz OLED, generic USB 3 "USB Video" capture card, PS5 at 1080p60:

| | |
|---|---|
| USB capture delay (card timestamp to app) | ~15–17 ms |
| App delay (frame received to Present) | ~0.5–1 ms |
| Dropped frames | 0 |
| Mouse to controller state, on the input thread | ~0.14 ms avg, 0.6 ms max (1000 Hz mouse) |
| Mouse maths per update | ~27 ns |

Most of the delay is the capture card itself, and the app can't fix that. The Remote Play sender sends stick updates at most every 8 ms. Button presses go out straight away.

## Requirements

- Windows 10 or 11, x64
- A UVC capture card that shows up as a normal camera (most cheap MS2109/MS2130 ones do)
- For mouse and keyboard: the PS5 and the PC on the same local network, and Remote Play turned on on the PS5
- To build: Visual Studio 2022 with "Desktop development with C++" and CMake 3.23 or newer

## Getting it

Grab the zip from [Releases](https://github.com/babymonie/PS5-DirectView/releases), or build it:

```bat
build.bat
```

or

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
build\Release\PS5DirectView.exe
```

Everything it needs is in `third_party/` and gets compiled in. Nothing is downloaded during the build.

## PS5 settings

- Settings > System > HDMI > Enable HDCP: **Off**, otherwise the capture card gets a black screen.
- Screen and Video: 1080p and 120 Hz off is the safe start for most cards.
- For MnK: Settings > System > Remote Play > Enable Remote Play.
- Optional, so the app can wake the console: Power Saving > Features Available in Rest Mode > Stay Connected to the Internet, and Enable Turning On PS5 from Network.

## Pairing (one time)

1. Open DirectView and press **P**, or click the controller icon at the top.
2. Click **Find PS5 on this network** and pick your console, or type its IP.
3. Enter your PSN account ID. This is the 12-character code ending in `=` (or the long number), not your sign-in name. chiaki-ng's account ID helper can show it to you. Use the account you actually play on.
4. On the PS5 go to Settings > System > Remote Play > **Pair Device** and type the 8-digit code it shows into DirectView.
5. Click **Pair with PS5**. It connects right after.

From then on it's just Connect (or turn on "connect when DirectView starts") and F8.

## Keys

| Key | |
|---|---|
| F8 | grab / release mouse and keyboard for the PS5 |
| Esc | release (never sent to the PS5) |
| P | controller settings |
| Tab | settings |
| F11 / Alt+Enter / double-click | fullscreen |
| H | stats overlay |
| C | crosshair |
| M | mute |
| S / R / V | sharpening / colour range / V-Sync |
| F5 | rescan capture devices |
| Ctrl+O | play a test video instead of the capture card |

Default controller mapping:

| Input | PS5 | Input | PS5 |
|---|---|---|---|
| W A S D | left stick | mouse | right stick |
| left click | R2 | right click | L2 |
| Space | Cross | Left Ctrl | Circle |
| R | Square | E | Triangle |
| Q | L1 | F | R1 |
| Left Shift | L3 | V / middle click | R3 |
| arrows, 1-4 | d-pad | Enter | Options |
| Backspace | Create | Tab | Touchpad |
| Home | PS button | hold Left Alt | walk (slower left stick) |

## Tuning aim

A stick controls how fast you turn, but a mouse reports how far it moved. So DirectView measures how fast the mouse is moving over the last few milliseconds and turns that into how far to push the stick. Some tips:

1. In the game, set stick sensitivity to max and the deadzone to min (if it has the option).
2. Raise **anti-deadzone** until moving the mouse slowly just barely starts turning the camera.
3. Then set **sensitivity** to taste.
4. Leave smoothing at 0 unless the aim feels jittery.

The curve preview in the settings shows exactly what each setting does.

This won't feel like native mouse aim. The game still sees a stick, with its own turn speed cap and acceleration. That's also why you still get aim assist.

## How it's put together

```
src/main.cpp            window, main loop, wiring
src/Capture.*           Media Foundation capture thread + single-frame mailbox
src/Renderer.*          D3D11 / DXGI flip model, GPU colour conversion, Direct2D overlay layer
src/Audio.*             WASAPI passthrough
src/Ui.*                the overlay UI and settings panel
src/InputManager.*      input thread: Raw Input, capture/release, key bindings
src/MouseTranslator.*   mouse speed -> right stick maths
src/InputSettings.*     aim settings and bindings (saved in the registry)
src/RemotePlayClient.*  find / wake / pair / connect / reconnect, input-only session
src/SecureStore.*       DPAPI storage for the pairing keys
tests/                  offline tests (input maths, Remote Play crypto) + a LAN discovery check
third_party/            chiaki-ng protocol core, mbedtls, nanopb, jerasure (see third_party/README.md)
```

Mouse and keyboard run on their own thread, so the capture and render threads don't touch them. While the mouse and keyboard are grabbed, the main window doesn't get any mouse or keyboard messages at all.

## GitHub Actions

- **Build** (`.github/workflows/build-windows.yml`) runs on every push to `main` and on every pull request. It builds the Release exe, runs the input tests and the crypto tests, and uploads the exe as a build artifact.
- **Release** (`.github/workflows/release.yml`) runs when you push a tag that starts with `v`. It builds, tests, zips the exe with the README, licence and test video, and publishes a GitHub release with a SHA-256 file:

  ```bash
  git tag v1.1.0
  git push origin v1.1.0
  ```

Running the tests locally:

```bat
cmake --build build --config Release --target InputTests CryptoTests
build\Release\InputTests.exe
build\Release\CryptoTests.exe
```

## Known limits

- Remote Play input only works on your local network, not over the internet.
- Only one Remote Play client can be connected at a time, so close PS Remote Play or chiaki elsewhere first.
- After a session ends the PS5 wants about 10 seconds before it accepts a new one. Auto-reconnect waits for that.
- If your PS5 user has a login passcode, turn it off. Entering it isn't supported.
- No rumble, gyro or touchpad position. The touchpad button does work.
- Video is 8-bit SDR only.
- Remote Play isn't an official API. A PS5 firmware update can break it until the bundled chiaki-ng code is updated (steps are in `third_party/README.md`).

## Fair play

Using a mouse and keyboard through controller emulation can break the rules of some online games. That's on you to check. This project has no macros, recoil scripts, aimbots or anything that plays for you. It just maps keys and the mouse to a controller.

## Credits and licence

The Remote Play protocol code comes from [chiaki-ng](https://github.com/streetpea/chiaki-ng), built from chiaki by Florian Märkl and others. That project did all the hard reverse engineering work. Crypto comes from Mbed TLS, protobuf from nanopb, and FEC from jerasure / gf-complete.

Because chiaki-ng is AGPL-3.0, this project is AGPL-3.0 too (see `LICENSE`). If you share builds, share the source with them.

Not affiliated with Sony. PlayStation, PS5 and Remote Play are trademarks of Sony Interactive Entertainment.
