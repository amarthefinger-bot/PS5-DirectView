#pragma once

#include "Capture.h"
#include "FrameGeneration.h"

#include <functional>
#include <memory>

#ifdef _WIN32
  #include <windows.h>
  #include <d2d1_1.h>
  #include <d3d11.h>
  #include <dxgi1_5.h>
  #include <mfapi.h>
  #include <wrl/client.h>
#endif

#ifdef _WIN32

class D3DRenderer {
public:
    void Initialize(HWND window);
    void Resize(unsigned width, unsigned height);
    void ResetVideo();
    void Render(const VideoFrame& frame);
    void Redraw();
    void SetFrameGeneration(bool enabled) { if (frameGeneration_ != enabled) { frameGeneration_ = enabled; cadence_.Reset(); } }
    bool FrameGeneration() const { return frameGeneration_; }
    void SetFsrUpscaling(bool enabled) { fsrUpscaling_ = enabled; }
    bool FsrUpscaling() const { return fsrUpscaling_; }
    using OverlayFn = std::function<void(ID2D1DeviceContext*, const D2D1_RECT_F& videoRect)>;
    void SetOverlay(OverlayFn fn) { overlay_ = std::move(fn); }
    ID2D1DeviceContext* D2D() const { return d2d_.Get(); }
    void SetDpi(float dpi);
    float Dpi() const { return dpi_; }
    bool HasImage() const { return hasImage_; }
    void SetVSync(bool enabled) { vsync_ = enabled; }
    bool VSync() const { return vsync_; }
    bool TearingSupported() const { return allowTearing_; }
    IMFDXGIDeviceManager* DeviceManager() const { return deviceManager_.Get(); }
    void SetRangeOverride(int mode) { rangeOverride_ = mode; paramsDirty_ = true; }
    int RangeOverride() const { return rangeOverride_; }
    void SetSharpness(float amount) { sharpness_ = amount; paramsDirty_ = true; }
    float Sharpness() const { return sharpness_; }

private:
    friend struct D3DRendererTestAccess;
    void CreateRenderTarget();
    void EnsureTextures(const FrameFormat& format);
    bool Upload(const VideoFrame& frame);
    bool CopyGpuFrame(const VideoFrame& frame);
    void UpdateParams(const FrameFormat& format);
    void Convert(ID3D11ShaderResourceView* view0, ID3D11ShaderResourceView* view1);
    void Present();
    void GenerateMidpoint();
    Microsoft::WRL::ComPtr<ID3D11Texture2D> previousRgb_, motionTexture_, midpointTexture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> previousView_, motionView_, midpointView_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> motionTarget_, midpointTarget_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psMotion_, psMidpoint_;
    bool showMidpoint_ = false;

    HWND hwnd_ = nullptr;
    unsigned clientWidth_ = 0;
    unsigned clientHeight_ = 0;
    unsigned textureWidth_ = 0;
    unsigned textureHeight_ = 0;
    PixelFormat texturePixel_ = PixelFormat::BGRA;
    bool textureBt709_ = true;
    bool textureFullRange_ = false;
    bool fsrUpscaling_ = false;
    bool frameGeneration_ = false;
    FrameGenerationCadence cadence_;
    bool hasImage_ = false;
    bool paramsDirty_ = true;
    int rangeOverride_ = 0;
    float sharpness_ = 0.5f;
    FrameFormat lastFormat_;
    bool vsync_ = false;
    bool allowTearing_ = false;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psBgra_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psYuy2_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psNv12_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psPresent_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> yuvConstants_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> d2d_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> d2dTarget_;
    OverlayFn overlay_;
    float dpi_ = 96.0f;
    UINT deviceManagerToken_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> gpuFrame_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gpuView0_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gpuView1_;
    D3D11_TEXTURE2D_DESC gpuDesc_{};

    Microsoft::WRL::ComPtr<ID3D11Texture2D> plane0_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> plane0View_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> plane1_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> plane1View_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rgbTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rgbTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> rgbView_;
};

#else

// macOS renderer: Metal-backed. Opaque pimpl; `MetalView` is set by Ui_Mac.
class D3DRenderer {
public:
    D3DRenderer();
    ~D3DRenderer();
    D3DRenderer(const D3DRenderer&) = delete;
    D3DRenderer& operator=(const D3DRenderer&) = delete;

    // Takes an NSView* (actually MTKView*). Bridged as void* for header purity.
    void Initialize(void* metalView);
    void Resize(unsigned width, unsigned height);
    void ResetVideo();
    void Render(const VideoFrame& frame);
    void Redraw();
    void SetFrameGeneration(bool enabled) { if (frameGeneration_ != enabled) { frameGeneration_ = enabled; cadence_.Reset(); } }
    bool FrameGeneration() const { return frameGeneration_; }
    void SetFsrUpscaling(bool enabled) { fsrUpscaling_ = enabled; }
    bool FsrUpscaling() const { return fsrUpscaling_; }

    // Mac overlay: free-form draw callback into a CoreGraphics context
    // layered over the Metal swap chain. (See Renderer_Mac.mm for size.)
    struct Rect { float left, top, right, bottom; };
    using OverlayFn = std::function<void(void* cgContext, const Rect& videoRect)>;
    void SetOverlay(OverlayFn fn) { overlay_ = std::move(fn); }

    void SetDpi(float dpi) { dpi_ = dpi; }
    float Dpi() const { return dpi_; }
    bool HasImage() const { return hasImage_; }
    void SetVSync(bool enabled) { vsync_ = enabled; }
    bool VSync() const { return vsync_; }
    bool TearingSupported() const { return false; }
    void SetRangeOverride(int mode) { rangeOverride_ = mode; }
    int RangeOverride() const { return rangeOverride_; }
    void SetSharpness(float amount) { sharpness_ = amount; }
    float Sharpness() const { return sharpness_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    OverlayFn overlay_;
    float dpi_ = 96.0f;
    bool fsrUpscaling_ = false;
    bool frameGeneration_ = false;
    FrameGenerationCadence cadence_;
    bool hasImage_ = false;
    bool vsync_ = true;
    int rangeOverride_ = 0;
    float sharpness_ = 0.5f;
    friend struct Impl;
};

#endif
