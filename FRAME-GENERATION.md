# FSR spatial upscaling and motion interpolation

This experimental build adds FSR 1 EASU spatial upscaling and an independent
image-based motion interpolator. Neither changes PS5 game simulation or rendering.

## Controls

Windows: open Settings (Tab), Picture. F6 toggles FSR 1 EASU; F7 toggles motion
interpolation. macOS: View > FSR 1 Spatial Upscaling (Command-U), or View > Motion
Interpolation (Command-G). Both options start off each launch.

Use interpolation only for steady 30 fps content carried in a 59.94/60 Hz capture
feed. Leave the console/capture output at 60 Hz. A 60 Hz output already has room
for 30 real frames plus 30 generated frames; 90 Hz is unnecessary. For 90 unique
presentations per second from 30 real frames, two generated frames per real frame
would be required. This build inserts one midpoint per pair, not two.

## How it works and limits

The explicit 30-in-60 mode samples alternating captured frames. It estimates
bidirectional image motion on a 16-pixel grid with a +/-32-pixel search and
one-pixel refinement. It warps the two real images towards their midpoint and
rejects locally inconsistent matches, using the current image there. The next
capture tick presents the real endpoint. All image processing stays on the GPU,
using D3D11 on Windows and Metal on macOS. Frame-sequence gaps reseed history;
capture formats outside 59-61 fps pass through. The capture FPS counter still
counts received video frames, not unique game frames or generated frames.

This is fixed cadence, not automatic duplicate detection. It also samples a 60
fps HUD at 30 fps. Do not enable it for native 60 fps games, 120 Hz capture,
variable or unstable frame pacing. At startup or after drops it needs a fresh
pair. The interpolation timeline adds approximately 16.7 ms relative to immediate
presentation of each newly sampled real frame, plus GPU processing and display
queues. The two source game frames are about 33.3 ms apart. Audio is unchanged
and is not delayed to compensate. Controls retain the game's original 30 fps
response. This does not meet an 8 ms total latency target.

Fast turns, occlusion, particles, small text and scene cuts can produce artifacts
or fall back to real frames. Local confidence rejection is not a global cut
detector. Use the off switch when responsiveness matters more than smoothness.
This initial implementation is block matching, not a neural RIFE model or vendor
optical-flow hardware integration. Live GTA footage and sustained 1080p GPU
performance still need validation on the target device.

FSR 1 EASU activates only when both dimensions of the letterboxed image exceed
the capture resolution. At native size or when downscaling the original scaling
path remains active. The port preserves AMD's 12-tap EASU kernel with native
32-bit reciprocal/rsqrt operations and float constants. It uses EASU alone,
without RCAS; the existing sharpening control applies to the non-EASU path.
This is spatial upscaling, not FSR 2/3/4 temporal reconstruction or frame generation.

DLSS Super Resolution/Frame Generation, temporal FSR and MetalFX interpolation
expect engine resources such as depth and motion vectors, unavailable in an HDMI
capture. No DLSS option is advertised or implemented. An RTX 5080 can run this
D3D11 image-based interpolator, as can other compatible GPUs. The Metal source
is provided for Macs, without requiring NVIDIA hardware.

## Build and validation

Build Windows with the existing CMake / Visual Studio instructions. Build Mac
using README-MACOS.md on a Mac with Xcode. No additional runtime SDKs or downloads
are required. The AMD reference headers and license notice are under
third_party/fsr1; the adapted EASU kernels are embedded in the renderer sources.

Targets FrameGenerationTests (portable) and RendererFrameGenerationTests
(Windows) exercise cadence, drop recovery, synthetic GPU midpoint position,
FSR presentation, and disabling the feature. InputTests checks the existing
input path. Windows shaders also compile at runtime.

Windows release build, shader compilation and synthetic D3D11 tests were run.
macOS compilation, Metal shader compilation and live capture testing were not
available in this Windows environment. Treat the Mac implementation as unverified
until built and tested on a Mac.

Reference: https://github.com/GPUOpen-Effects/FidelityFX-FSR
