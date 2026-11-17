#include "Renderer.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <mfobjects.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {
void Check(HRESULT result, const char* where) {
    if (FAILED(result)) {
        char msg[128]{};
        sprintf_s(msg, "%s failed (HRESULT 0x%08lX)", where,
                  static_cast<unsigned long>(result));
        throw std::runtime_error(msg);
    }
}

const char* kShaders = R"HLSL(
struct VertexOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VertexOutput VSMain(uint vertexID : SV_VertexID) {
    VertexOutput v;
    // One giant fullscreen triangle, avoiding a vertex/index buffer.
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    v.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    v.uv = uv;
    return v;
}

Texture2D<float4> plane0 : register(t0);
Texture2D<float4> plane1 : register(t1);
SamplerState linearSampler : register(s0);

cbuffer YuvParams : register(b0) {
    float4 range;  // x: luma scale, y: luma offset, z: chroma scale, w: chroma offset
    float4 coeff;  // x: R<-V, y: G<-U, z: G<-V, w: B<-U
    float4 sharp;  // x: sharpening amount 0..1 (0 = off)
};

float3 YuvToRgb(float y, float u, float v) {
    y = (y - range.y) * range.x;
    u = (u - range.w) * range.z;
    v = (v - range.w) * range.z;
    return saturate(float3(y + coeff.x * v,
                           y + coeff.y * u + coeff.z * v,
                           y + coeff.w * u));
}

// Pass 1 shaders run at source resolution; one output pixel per input pixel.
float4 PSBgra(VertexOutput v) : SV_TARGET {
    int2 p = int2(v.position.xy);
    return float4(plane0.Load(int3(p, 0)).rgb, 1.0);
}

float4 PSYuy2(VertexOutput v) : SV_TARGET {
    // Each RGBA8 texel packs two pixels: Y0 U Y1 V.
    int2 p = int2(v.position.xy);
    float4 t = plane0.Load(int3(p.x >> 1, p.y, 0));
    float y = (p.x & 1) ? t.b : t.r;
    return float4(YuvToRgb(y, t.g, t.a), 1.0);
}

float4 PSNv12(VertexOutput v) : SV_TARGET {
    int2 p = int2(v.position.xy);
    float y = plane0.Load(int3(p, 0)).r;
    float2 c = plane1.Load(int3(p >> 1, 0)).rg;
    return float4(YuvToRgb(y, c.x, c.y), 1.0);
}

// Pass 2: scale the converted image into the letterboxed window area.
// Contrast-adaptive sharpening (after AMD FidelityFX CAS): restores edge
// detail lost to the card's scaler and MJPEG compression, while backing off
// on already-contrasty edges so it does not ring or amplify block noise.
float4 PSPresent(VertexOutput v) : SV_TARGET {
    float3 c = plane0.Sample(linearSampler, v.uv).rgb;
    if (sharp.x <= 0.0) return float4(c, 1.0);
    float w, h;
    plane0.GetDimensions(w, h);
    float2 t = float2(1.0 / w, 1.0 / h);
    float3 n = plane0.Sample(linearSampler, v.uv + float2(0, -t.y)).rgb;
    float3 s = plane0.Sample(linearSampler, v.uv + float2(0,  t.y)).rgb;
    float3 e = plane0.Sample(linearSampler, v.uv + float2( t.x, 0)).rgb;
    float3 o = plane0.Sample(linearSampler, v.uv + float2(-t.x, 0)).rgb;
    float3 mn = min(c, min(min(n, s), min(e, o)));
    float3 mx = max(c, max(max(n, s), max(e, o)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    float3 wgt = amp * (-1.0 / lerp(8.0, 5.0, sharp.x));
    float3 r = (c + (n + s + e + o) * wgt) / (1.0 + 4.0 * wgt);
    return float4(saturate(r), 1.0);
}
)HLSL";

struct YuvParams {
    float range[4];
    float coeff[4];
    float sharp[4];
};

YuvParams MakeYuvParams(bool bt709, bool fullRange) {
    YuvParams p{};
    if (fullRange) {
        p.range[0] = 1.0f; p.range[1] = 0.0f;
        p.range[2] = 1.0f; p.range[3] = 128.0f / 255.0f;
    } else {
        p.range[0] = 255.0f / 219.0f; p.range[1] = 16.0f / 255.0f;
        p.range[2] = 255.0f / 224.0f; p.range[3] = 128.0f / 255.0f;
    }
    if (bt709) {
        p.coeff[0] = 1.5748f; p.coeff[1] = -0.1873f;
        p.coeff[2] = -0.4681f; p.coeff[3] = 1.8556f;
    } else {
        p.coeff[0] = 1.402f; p.coeff[1] = -0.344136f;
        p.coeff[2] = -0.714136f; p.coeff[3] = 1.772f;
    }
    return p;
}

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT hr = D3DCompile(kShaders, std::strlen(kShaders), "DirectView.hlsl",
                                  nullptr, nullptr, entry, target, flags, 0,
                                  &bytecode, &errors);
    if (FAILED(hr)) {
        std::string message = std::string("Shader compilation failed (") + entry + ")";
        if (errors) message += ": " + std::string(
            static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        throw std::runtime_error(message);
    }
    return bytecode;
}

void CopyRows(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
              const BYTE* top, long long pitch, size_t rowBytes, unsigned rows) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(context->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
          "Map(video plane)");
    auto* dst = static_cast<std::uint8_t*>(mapped.pData);
    if (pitch == static_cast<long long>(mapped.RowPitch) &&
        static_cast<size_t>(pitch) == rowBytes) {
        std::memcpy(dst, top, rowBytes * rows); // tightly packed: one copy
    } else {
        for (unsigned y = 0; y < rows; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
                        top + static_cast<long long>(y) * pitch, rowBytes);
    }
    context->Unmap(texture, 0);
}
} // namespace

void D3DRenderer::Initialize(HWND window) {
    hwnd_ = window;
    RECT rect{};
    GetClientRect(window, &rect);
    clientWidth_ = static_cast<unsigned>(std::max<LONG>(1, rect.right - rect.left));
    clientHeight_ = static_cast<unsigned>(std::max<LONG>(1, rect.bottom - rect.top));

    ComPtr<IDXGIFactory2> factory;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    // Use the GPU that drives the window's monitor (e.g. the RTX card, not
    // the CPU's iGPU), so decode, convert and scan-out never cross adapters.
    ComPtr<IDXGIAdapter1> adapter;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> candidate;
    for (UINT a = 0; !adapter && factory->EnumAdapters1(a, &candidate) != DXGI_ERROR_NOT_FOUND; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; candidate->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(output->GetDesc(&od)) && od.Monitor == monitor) {
                adapter = candidate;
                break;
            }
            output.Reset();
        }
        candidate.Reset();
    }

    // VIDEO_SUPPORT lets Media Foundation run hardware decoders on this device.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL actual{};
    const D3D_DRIVER_TYPE driverType = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT created = D3D11CreateDevice(adapter.Get(), driverType, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, &actual, &context_);
    if (FAILED(created))
        created = D3D11CreateDevice(adapter.Get(), driverType, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION, &device_, &actual, &context_);
    Check(created, "D3D11CreateDevice");

    // The capture thread's decoder and this thread's renderer share the
    // immediate context; serialize access to it.
    ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread))) multithread->SetMultithreadProtected(TRUE);
    if (SUCCEEDED(MFCreateDXGIDeviceManager(&deviceManagerToken_, &deviceManager_)) &&
        FAILED(deviceManager_->ResetDevice(device_.Get(), deviceManagerToken_)))
        deviceManager_.Reset();
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL supportsTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supportsTearing,
                sizeof(supportsTearing))))
            allowTearing_ = supportsTearing == TRUE;
    }

    // Avoid queueing frames ahead of display, and ask the GPU scheduler to
    // favour this process (ignored without privilege; harmless).
    ComPtr<IDXGIDevice1> dxgiDevice;
    if (SUCCEEDED(device_.As(&dxgiDevice))) {
        dxgiDevice->SetMaximumFrameLatency(1);
        dxgiDevice->SetGPUThreadPriority(7);
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = clientWidth_;
    desc.Height = clientHeight_;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    Check(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc,
                                          nullptr, nullptr, &swapChain_),
          "CreateSwapChainForHwnd");
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    // Direct2D on the same device: UI is composited into the back buffer by
    // the GPU in the same Present, so no extra windows or DWM layers.
    D2D1_FACTORY_OPTIONS d2dOptions{};
    Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1),
                            &d2dOptions, reinterpret_cast<void**>(d2dFactory_.GetAddressOf())),
          "D2D1CreateFactory");
    ComPtr<IDXGIDevice> dxgiForD2D;
    Check(device_.As(&dxgiForD2D), "IDXGIDevice");
    Check(d2dFactory_->CreateDevice(dxgiForD2D.Get(), &d2dDevice_), "ID2D1Factory1::CreateDevice");
    Check(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_),
          "CreateDeviceContext");
    dpi_ = static_cast<float>(GetDpiForWindow(hwnd_));
    d2d_->SetDpi(dpi_, dpi_);
    CreateRenderTarget();

    auto vs = Compile("VSMain", "vs_5_0");
    Check(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(),
                                     nullptr, &vertexShader_), "CreateVertexShader");
    struct { const char* entry; ComPtr<ID3D11PixelShader>* target; } pixelShaders[] = {
        {"PSBgra", &psBgra_}, {"PSYuy2", &psYuy2_},
        {"PSNv12", &psNv12_}, {"PSPresent", &psPresent_},
    };
    for (auto& ps : pixelShaders) {
        auto blob = Compile(ps.entry, "ps_5_0");
        Check(device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                        nullptr, ps.target->GetAddressOf()),
              "CreatePixelShader");
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    Check(device_->CreateSamplerState(&samplerDesc, &sampler_), "CreateSamplerState");

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(YuvParams);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const YuvParams initial = MakeYuvParams(true, false);
    D3D11_SUBRESOURCE_DATA init{&initial, 0, 0};
    Check(device_->CreateBuffer(&cb, &init, &yuvConstants_), "CreateBuffer(constants)");
}

void D3DRenderer::CreateRenderTarget() {
    ComPtr<ID3D11Texture2D> buffer;
    Check(swapChain_->GetBuffer(0, IID_PPV_ARGS(&buffer)), "GetBuffer");
    Check(device_->CreateRenderTargetView(buffer.Get(), nullptr, &rtv_),
          "CreateRenderTargetView");
    if (d2d_) {
        ComPtr<IDXGISurface> surface;
        Check(buffer.As(&surface), "Back buffer surface");
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi_, dpi_);
        Check(d2d_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &d2dTarget_),
              "CreateBitmapFromDxgiSurface");
    }
}

void D3DRenderer::SetDpi(float dpi) {
    if (!d2d_ || dpi == dpi_) return;
    dpi_ = dpi;
    d2d_->SetDpi(dpi_, dpi_);
    d2d_->SetTarget(nullptr);
    d2dTarget_.Reset();
    rtv_.Reset();
    CreateRenderTarget();
}

void D3DRenderer::Resize(unsigned width, unsigned height) {
    if (!swapChain_ || width == 0 || height == 0) return;
    if (width == clientWidth_ && height == clientHeight_) return;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    if (d2d_) d2d_->SetTarget(nullptr);
    d2dTarget_.Reset();
    // Release references to old backbuffer before resize.
    context_->Flush();
    Check(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                   allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0),
          "ResizeBuffers");
    clientWidth_ = width;
    clientHeight_ = height;
    CreateRenderTarget();
}

void D3DRenderer::ResetVideo() {
    if (context_) {
        ID3D11ShaderResourceView* nullViews[2] = {};
        context_->PSSetShaderResources(0, 2, nullViews);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
    }
    plane0View_.Reset();
    plane0_.Reset();
    plane1View_.Reset();
    plane1_.Reset();
    gpuView0_.Reset();
    gpuView1_.Reset();
    gpuFrame_.Reset();
    gpuDesc_ = {};
    rgbView_.Reset();
    rgbTarget_.Reset();
    rgbTexture_.Reset();
    textureWidth_ = textureHeight_ = 0;
    hasImage_ = false;
}

void D3DRenderer::UpdateParams(const FrameFormat& format) {
    const bool fullRange = rangeOverride_ == 1 ? true :
                           rangeOverride_ == 2 ? false : format.fullRange;
    if (!paramsDirty_ && format.bt709 == textureBt709_ && fullRange == textureFullRange_)
        return;
    YuvParams params = MakeYuvParams(format.bt709, fullRange);
    params.sharp[0] = sharpness_;
    context_->UpdateSubresource(yuvConstants_.Get(), 0, nullptr, &params, 0, 0);
    textureBt709_ = format.bt709;
    textureFullRange_ = fullRange;
    paramsDirty_ = false;
}

bool D3DRenderer::CopyGpuFrame(const VideoFrame& frame) {
    ComPtr<IMFDXGIBuffer> dxgi;
    if (FAILED(frame.buffer.As(&dxgi))) return false;
    ComPtr<ID3D11Texture2D> source;
    UINT subresource = 0;
    if (FAILED(dxgi->GetResource(IID_PPV_ARGS(&source))) ||
        FAILED(dxgi->GetSubresourceIndex(&subresource))) return false;
    ComPtr<ID3D11Device> owner;
    source->GetDevice(&owner);
    if (owner.Get() != device_.Get()) return false; // fall back to CPU readback

    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    PixelFormat pixel;
    DXGI_FORMAT view0 = DXGI_FORMAT_UNKNOWN, view1 = DXGI_FORMAT_UNKNOWN;
    switch (desc.Format) {
        case DXGI_FORMAT_NV12:
            pixel = PixelFormat::NV12; view0 = DXGI_FORMAT_R8_UNORM; view1 = DXGI_FORMAT_R8G8_UNORM;
            break;
        case DXGI_FORMAT_YUY2:
            pixel = PixelFormat::YUY2; view0 = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
            pixel = PixelFormat::BGRA; view0 = desc.Format;
            break;
        default:
            return false;
    }
    if (desc.Width < textureWidth_ || desc.Height < textureHeight_) return false;

    if (!gpuFrame_ || gpuDesc_.Width != desc.Width || gpuDesc_.Height != desc.Height ||
        gpuDesc_.Format != desc.Format) {
        gpuView0_.Reset(); gpuView1_.Reset(); gpuFrame_.Reset();
        D3D11_TEXTURE2D_DESC copy{};
        copy.Width = desc.Width;
        copy.Height = desc.Height;
        copy.MipLevels = 1;
        copy.ArraySize = 1;
        copy.Format = desc.Format;
        copy.SampleDesc.Count = 1;
        copy.Usage = D3D11_USAGE_DEFAULT;
        copy.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&copy, nullptr, &gpuFrame_))) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        sv.Format = view0;
        if (FAILED(device_->CreateShaderResourceView(gpuFrame_.Get(), &sv, &gpuView0_))) {
            gpuFrame_.Reset();
            return false;
        }
        if (view1 != DXGI_FORMAT_UNKNOWN) {
            sv.Format = view1;
            if (FAILED(device_->CreateShaderResourceView(gpuFrame_.Get(), &sv, &gpuView1_))) {
                gpuView0_.Reset(); gpuFrame_.Reset();
                return false;
            }
        }
        gpuDesc_ = copy;
    }
    // GPU-to-GPU copy out of the decoder's texture array (microseconds), so
    // the decoder surface can return to its pool right away.
    context_->CopySubresourceRegion(gpuFrame_.Get(), 0, 0, 0, 0,
                                    source.Get(), subresource, nullptr);
    texturePixel_ = pixel;
    return true;
}

void D3DRenderer::EnsureTextures(const FrameFormat& format) {
    if (rgbTexture_ && format.width == textureWidth_ &&
        format.height == textureHeight_ && format.pixel == texturePixel_) return;
    ResetVideo();

    auto makePlane = [&](unsigned w, unsigned h, DXGI_FORMAT fmt,
                         ComPtr<ID3D11Texture2D>& tex,
                         ComPtr<ID3D11ShaderResourceView>& view) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w;
        desc.Height = h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = fmt;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        Check(device_->CreateTexture2D(&desc, nullptr, &tex), "CreateTexture2D(plane)");
        Check(device_->CreateShaderResourceView(tex.Get(), nullptr, &view),
              "CreateShaderResourceView(plane)");
    };
    switch (format.pixel) {
        case PixelFormat::YUY2:
            makePlane(format.width / 2, format.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                      plane0_, plane0View_);
            break;
        case PixelFormat::NV12:
            makePlane(format.width, format.height, DXGI_FORMAT_R8_UNORM,
                      plane0_, plane0View_);
            makePlane(format.width / 2, format.height / 2, DXGI_FORMAT_R8G8_UNORM,
                      plane1_, plane1View_);
            break;
        default:
            makePlane(format.width, format.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                      plane0_, plane0View_);
            break;
    }

    D3D11_TEXTURE2D_DESC rgb{};
    rgb.Width = format.width;
    rgb.Height = format.height;
    rgb.MipLevels = 1;
    rgb.ArraySize = 1;
    rgb.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rgb.SampleDesc.Count = 1;
    rgb.Usage = D3D11_USAGE_DEFAULT;
    rgb.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Check(device_->CreateTexture2D(&rgb, nullptr, &rgbTexture_), "CreateTexture2D(rgb)");
    Check(device_->CreateRenderTargetView(rgbTexture_.Get(), nullptr, &rgbTarget_),
          "CreateRenderTargetView(rgb)");
    Check(device_->CreateShaderResourceView(rgbTexture_.Get(), nullptr, &rgbView_),
          "CreateShaderResourceView(rgb)");

    textureWidth_ = format.width;
    textureHeight_ = format.height;
    texturePixel_ = format.pixel;
}

bool D3DRenderer::Upload(const VideoFrame& frame) {
    const FrameFormat& f = frame.format;
    IMFMediaBuffer* buffer = frame.buffer.Get();

    const BYTE* top = nullptr;      // first displayed scanline
    long long pitch = 0;            // bytes between displayed scanlines
    const BYTE* begin = nullptr;    // valid memory range, when known
    const BYTE* end = nullptr;
    ComPtr<IMF2DBuffer2> twoD2;
    ComPtr<IMF2DBuffer> twoD;
    BYTE* raw = nullptr;
    enum { kNone, k2D2, k2D, kLinear } lockKind = kNone;

    // Lock2D/Lock2DSize return the top scanline even for bottom-up images
    // and avoid the internal copy a plain Lock() can make on GPU buffers.
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&twoD2)))) {
        BYTE* scan0 = nullptr; LONG p = 0; BYTE* start = nullptr; DWORD length = 0;
        if (SUCCEEDED(twoD2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &p,
                                        &start, &length))) {
            lockKind = k2D2;
            top = scan0; pitch = p; begin = start; end = start + length;
        }
    }
    if (lockKind == kNone && SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&twoD)))) {
        BYTE* scan0 = nullptr; LONG p = 0;
        if (SUCCEEDED(twoD->Lock2D(&scan0, &p))) {
            lockKind = k2D;
            top = scan0; pitch = p;
        }
    }
    if (lockKind == kNone) {
        DWORD maxLength = 0, currentLength = 0;
        if (FAILED(buffer->Lock(&raw, &maxLength, &currentLength))) return false;
        lockKind = kLinear;
        pitch = f.stride;
        const long long absPitch = pitch < 0 ? -pitch : pitch;
        // In a linear bottom-up buffer the first byte is the bottom scanline.
        top = pitch < 0 ? raw + (f.height - 1) * absPitch : raw;
        begin = raw; end = raw + currentLength;
    }

    const long long absPitch = pitch < 0 ? -pitch : pitch;
    unsigned bytesPerPixel = f.pixel == PixelFormat::BGRA ? 4 :
                             f.pixel == PixelFormat::YUY2 ? 2 : 1;
    const size_t rowBytes = static_cast<size_t>(f.width) * bytesPerPixel;
    bool ok = top && absPitch >= static_cast<long long>(rowBytes) &&
              (f.pixel == PixelFormat::BGRA || pitch > 0);

    // NV12's chroma plane follows the luma plane. Decoders sometimes pad the
    // luma plane height (e.g. 1088 for 1080), which the buffer length reveals.
    unsigned lumaRows = std::max(f.height, f.codedHeight);
    if (ok && f.pixel == PixelFormat::NV12 && begin && end > top) {
        const long long available = end - top;
        const long long padded = (available * 2) / (absPitch * 3);
        if (padded > f.height && padded <= f.height + 64)
            lumaRows = static_cast<unsigned>(padded);
    }
    if (ok && begin && end) {
        // Bounds check against the buffer's real extent.
        const BYTE* lowest = pitch < 0 ? top - (f.height - 1) * absPitch : top;
        long long span = absPitch * f.height;
        if (f.pixel == PixelFormat::NV12)
            span = absPitch * lumaRows + absPitch * (f.height / 2);
        ok = lowest >= begin && lowest + span <= end;
    }

    if (ok) {
        CopyRows(context_.Get(), plane0_.Get(), top, pitch, rowBytes, f.height);
        if (f.pixel == PixelFormat::NV12)
            CopyRows(context_.Get(), plane1_.Get(),
                     top + absPitch * lumaRows, pitch, rowBytes, f.height / 2);
    }

    if (lockKind == k2D2) twoD2->Unlock2D();
    else if (lockKind == k2D) twoD->Unlock2D();
    else buffer->Unlock();
    return ok;
}

void D3DRenderer::Convert(ID3D11ShaderResourceView* view0, ID3D11ShaderResourceView* view1) {
    // Direct2D leaves its own blend/raster state behind; start clean.
    context_->ClearState();
    ID3D11ShaderResourceView* nullViews[2] = {};
    context_->PSSetShaderResources(0, 2, nullViews);
    ID3D11RenderTargetView* targets[] = {rgbTarget_.Get()};
    context_->OMSetRenderTargets(1, targets, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(textureWidth_);
    viewport.Height = static_cast<float>(textureHeight_);
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);

    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    ID3D11PixelShader* ps = texturePixel_ == PixelFormat::YUY2 ? psYuy2_.Get() :
                            texturePixel_ == PixelFormat::NV12 ? psNv12_.Get() :
                                                                 psBgra_.Get();
    context_->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* views[] = {view0, view1};
    context_->PSSetShaderResources(0, 2, views);
    ID3D11Buffer* constants[] = {yuvConstants_.Get()};
    context_->PSSetConstantBuffers(0, 1, constants);
    context_->Draw(3, 0);
    hasImage_ = true;
}

void D3DRenderer::Present() {
    if (!swapChain_ || !rtv_ || !clientWidth_ || !clientHeight_) return;
    context_->ClearState();
    D2D1_RECT_F videoDip = D2D1::RectF(0, 0, clientWidth_ * 96.0f / dpi_,
                                      clientHeight_ * 96.0f / dpi_);
    ID3D11ShaderResourceView* nullViews[2] = {};
    context_->PSSetShaderResources(0, 2, nullViews);
    const float black[] = {0.0f, 0.0f, 0.0f, 1.0f};
    ID3D11RenderTargetView* targets[] = {rtv_.Get()};
    context_->OMSetRenderTargets(1, targets, nullptr);
    context_->ClearRenderTargetView(rtv_.Get(), black);

    if (hasImage_ && rgbView_) {
        // Letterbox/pillarbox instead of stretching 16:9 footage on an ultrawide.
        const double sx = static_cast<double>(clientWidth_) / textureWidth_;
        const double sy = static_cast<double>(clientHeight_) / textureHeight_;
        const double scale = std::min(sx, sy);
        const auto vw = static_cast<float>(textureWidth_ * scale);
        const auto vh = static_cast<float>(textureHeight_ * scale);
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = (clientWidth_ - vw) * 0.5f;
        viewport.TopLeftY = (clientHeight_ - vh) * 0.5f;
        viewport.Width = vw;
        viewport.Height = vh;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        const float toDip = 96.0f / dpi_;
        videoDip = D2D1::RectF(viewport.TopLeftX * toDip, viewport.TopLeftY * toDip,
                               (viewport.TopLeftX + vw) * toDip,
                               (viewport.TopLeftY + vh) * toDip);

        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(psPresent_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[] = {rgbView_.Get()};
        ID3D11SamplerState* samplers[] = {sampler_.Get()};
        context_->PSSetShaderResources(0, 1, views);
        context_->PSSetSamplers(0, 1, samplers);
        ID3D11Buffer* constants[] = {yuvConstants_.Get()};
        context_->PSSetConstantBuffers(0, 1, constants);
        context_->Draw(3, 0);
    }

    if (overlay_ && d2dTarget_) {
        d2d_->SetTarget(d2dTarget_.Get());
        d2d_->BeginDraw();
        d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
        overlay_(d2d_.Get(), videoDip);
        d2d_->EndDraw();
        d2d_->SetTarget(nullptr);
    }

    // DXGI tearing, if available, avoids waiting for the next display refresh.
    const UINT syncInterval = vsync_ ? 1 : 0;
    const UINT flags = (!vsync_ && allowTearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    Check(swapChain_->Present(syncInterval, flags), "Present");
}

void D3DRenderer::Render(const VideoFrame& frame) {
    const FrameFormat& f = frame.format;
    if (!swapChain_ || !frame.buffer || !f.width || !f.height) return;
    EnsureTextures(f);
    lastFormat_ = f;
    UpdateParams(f);
    if (CopyGpuFrame(frame)) {
        // Hardware-decoded: frame never touched system memory.
        Convert(gpuView0_.Get(), gpuView1_.Get());
    } else {
        texturePixel_ = f.pixel;
        if (!Upload(frame)) return;
        Convert(plane0View_.Get(), plane1View_.Get());
    }
    Present();
}

void D3DRenderer::Redraw() {
    if (hasImage_ && paramsDirty_) UpdateParams(lastFormat_);
    Present(); // also draws the UI over black when there is no video
}
