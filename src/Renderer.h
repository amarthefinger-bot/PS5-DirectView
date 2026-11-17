#pragma once

#include "Capture.h"
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <functional>
#include <dxgi1_5.h>
#include <mfapi.h>
#include <wrl/client.h>

class D3DRenderer {
public:
    void Initialize(HWND window);
    void Resize(unsigned width, unsigned height);
    void ResetVideo();
    // Uploads the frame's planes, converts to RGB on the GPU, and presents.
    void Render(const VideoFrame& frame);
    // Re-presents the last converted frame (window resize, V-Sync toggle).
    void Redraw();
    // UI/overlay layer drawn with Direct2D into the same back buffer, right
    // before Present. Coordinates are DIPs; videoRect is where video landed.
    using OverlayFn = std::function<void(ID2D1DeviceContext*, const D2D1_RECT_F& videoRect)>;
    void SetOverlay(OverlayFn fn) { overlay_ = std::move(fn); }
    ID2D1DeviceContext* D2D() const { return d2d_.Get(); }
    void SetDpi(float dpi);
    float Dpi() const { return dpi_; }
    bool HasImage() const { return hasImage_; }
    void SetVSync(bool enabled) { vsync_ = enabled; }
    bool VSync() const { return vsync_; }
    bool TearingSupported() const { return allowTearing_; }
    // Shared with Media Foundation so hardware decoders output on this GPU.
    IMFDXGIDeviceManager* DeviceManager() const { return deviceManager_.Get(); }
    // 0 = auto (from stream), 1 = force full range, 2 = force limited range.
    void SetRangeOverride(int mode) { rangeOverride_ = mode; paramsDirty_ = true; }
    int RangeOverride() const { return rangeOverride_; }
    // 0 = off .. 1 = strongest contrast-adaptive sharpening.
    void SetSharpness(float amount) { sharpness_ = amount; paramsDirty_ = true; }
    float Sharpness() const { return sharpness_; }

private:
    void CreateRenderTarget();
    void EnsureTextures(const FrameFormat& format);
    bool Upload(const VideoFrame& frame);
    bool CopyGpuFrame(const VideoFrame& frame);
    void UpdateParams(const FrameFormat& format);
    void Convert(ID3D11ShaderResourceView* view0, ID3D11ShaderResourceView* view1);
    void Present();

    HWND hwnd_ = nullptr;
    unsigned clientWidth_ = 0;
    unsigned clientHeight_ = 0;
    unsigned textureWidth_ = 0;
    unsigned textureHeight_ = 0;
    PixelFormat texturePixel_ = PixelFormat::BGRA;
    bool textureBt709_ = true;
    bool textureFullRange_ = false;
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

    // GPU path: copy of the hardware decoder's output surface.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> gpuFrame_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gpuView0_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> gpuView1_;
    D3D11_TEXTURE2D_DESC gpuDesc_{};

    // Raw capture planes (CPU-writable).
    Microsoft::WRL::ComPtr<ID3D11Texture2D> plane0_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> plane0View_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> plane1_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> plane1View_;
    // Converted RGB image at source resolution, scaled to the window on present.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rgbTexture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rgbTarget_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> rgbView_;
};
