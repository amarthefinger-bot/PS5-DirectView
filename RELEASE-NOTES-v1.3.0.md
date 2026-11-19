# v1.3.0

- Fix the macOS fullscreen crash caused by recursively setting MTKView drawableSize from its resize notification. MetalKit now owns automatic sizing, including Retina and fullscreen transitions.
- Add optional FSR 1 EASU spatial upscaling to the D3D11 and Metal renderers. It activates when enlarging the captured image. This does not include RCAS or temporal FSR.
- Add experimental GPU image-based motion interpolation for steady 30 FPS content inside a 59.94/60 Hz capture feed. Windows: F7 or Settings > Picture. Mac: View > Motion Interpolation.
- Windows F6 and Mac View > FSR 1 Spatial Upscaling toggle EASU. Both new modes start disabled.
- Reset interpolation history after capture-frame gaps and bypass unsupported capture rates.

The interpolation mode samples alternating captured frames. It is not automatic
duplicate detection and should stay off for native 60 FPS gameplay. Generated
frames add visual smoothness, delay and possible artifacts, without increasing
the game's simulation FPS or input responsiveness. No DLSS support is advertised:
HDMI capture does not expose the game-engine buffers it requires.

Validation: Windows release build; all seven HLSL shaders compiled; synthetic
D3D11 midpoint tests at 256x128 and 1920x1080; cadence/drop-recovery tests; existing
input tests (33 checks, zero failures). The Mac fullscreen fix follows the
supplied crash report and source feedback loop. Live fullscreen, monitor changes,
Metal shader execution and GTA capture still need validation on a Mac.
