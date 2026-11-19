#include "Renderer.h"

#ifdef _WIN32
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

// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// EASU port: native reciprocal/rsqrt and float constants.
float safeRcp(float x) { return 1.0 / max(x,1e-8); }
float safeRsqrt(float x) { return rsqrt(max(x,1e-8)); }
float3 min3(float3 a,float3 b,float3 c) { return min(a,min(b,c)); }
float3 max3(float3 a,float3 b,float3 c) { return max(a,max(b,c)); }

float4 FsrEasuRF(float2 p) { return plane0.GatherRed(linearSampler,p); }
float4 FsrEasuGF(float2 p) { return plane0.GatherGreen(linearSampler,p); }
float4 FsrEasuBF(float2 p) { return plane0.GatherBlue(linearSampler,p); }
 void FsrEasuTapF(
 inout float3 aC, 
 inout float aW, 
 float2 off, 
 float2 dir, 
 float2 len, 
 float lob, 
 float clp, 
 float3 c){ 
  
  float2 v;
  v.x=(off.x*( dir.x))+(off.y*dir.y);
  v.y=(off.x*(-dir.y))+(off.y*dir.x);
  
  v*=len;
  
  float d2=v.x*v.x+v.y*v.y;
  
  d2=min(d2,clp);
  
  
  
  
  
  
  
  float wB=float(2.0/5.0)*d2+float(-1.0);
  float wA=lob*d2+float(-1.0);
  wB*=wB;
  wA*=wA;
  wB=float(25.0/16.0)*wB+float(-(25.0/16.0-1.0));
  float w=wB*wA;
  
  aC+=c*w;aW+=w;}

 
 void FsrEasuSetF(
 inout float2 dir,
 inout float len,
 float2 pp,
 bool biS,bool biT,bool biU,bool biV,
 float lA,float lB,float lC,float lD,float lE){
  
  
  
  float w = float(0.0);
  if(biS)w=(float(1.0)-pp.x)*(float(1.0)-pp.y);
  if(biT)w=           pp.x *(float(1.0)-pp.y);
  if(biU)w=(float(1.0)-pp.x)*           pp.y ;
  if(biV)w=           pp.x *           pp.y ;
  
  
  
  
  
  
  float dc=lD-lC;
  float cb=lC-lB;
  float lenX=max(abs(dc),abs(cb));
  lenX=safeRcp(lenX);
  float dirX=lD-lB;
  dir.x+=dirX*w;
  lenX=saturate(abs(dirX)*lenX);
  lenX*=lenX;
  len+=lenX*w;
  
  float ec=lE-lC;
  float ca=lC-lA;
  float lenY=max(abs(ec),abs(ca));
  lenY=safeRcp(lenY);
  float dirY=lE-lA;
  dir.y+=dirY*w;
  lenY=saturate(abs(dirY)*lenY);
  lenY*=lenY;
  len+=lenY*w;}

 void FsrEasuF(
 out float3 pix,
 uint2 ip, 
 float4 con0, 
 float4 con1,
 float4 con2,
 float4 con3){

  
  float2 pp=float2(ip)*con0.xy+con0.zw;
  float2 fp=floor(pp);
  pp-=fp;

  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  float2 p0=fp*con1.xy+con1.zw;
  
  float2 p1=p0+con2.xy;
  float2 p2=p0+con2.zw;
  float2 p3=p0+con3.xy;
  float4 bczzR=FsrEasuRF(p0);
  float4 bczzG=FsrEasuGF(p0);
  float4 bczzB=FsrEasuBF(p0);
  float4 ijfeR=FsrEasuRF(p1);
  float4 ijfeG=FsrEasuGF(p1);
  float4 ijfeB=FsrEasuBF(p1);
  float4 klhgR=FsrEasuRF(p2);
  float4 klhgG=FsrEasuGF(p2);
  float4 klhgB=FsrEasuBF(p2);
  float4 zzonR=FsrEasuRF(p3);
  float4 zzonG=FsrEasuGF(p3);
  float4 zzonB=FsrEasuBF(p3);

  
  float4 bczzL=bczzB*float4(0.5,0.5,0.5,0.5)+(bczzR*float4(0.5,0.5,0.5,0.5)+bczzG);
  float4 ijfeL=ijfeB*float4(0.5,0.5,0.5,0.5)+(ijfeR*float4(0.5,0.5,0.5,0.5)+ijfeG);
  float4 klhgL=klhgB*float4(0.5,0.5,0.5,0.5)+(klhgR*float4(0.5,0.5,0.5,0.5)+klhgG);
  float4 zzonL=zzonB*float4(0.5,0.5,0.5,0.5)+(zzonR*float4(0.5,0.5,0.5,0.5)+zzonG);
  
  float bL=bczzL.x;
  float cL=bczzL.y;
  float iL=ijfeL.x;
  float jL=ijfeL.y;
  float fL=ijfeL.z;
  float eL=ijfeL.w;
  float kL=klhgL.x;
  float lL=klhgL.y;
  float hL=klhgL.z;
  float gL=klhgL.w;
  float oL=zzonL.z;
  float nL=zzonL.w;
  
  float2 dir=float2(0.0,0.0);
  float len=float(0.0);
  FsrEasuSetF(dir,len,pp,true, false,false,false,bL,eL,fL,gL,jL);
  FsrEasuSetF(dir,len,pp,false,true ,false,false,cL,fL,gL,hL,kL);
  FsrEasuSetF(dir,len,pp,false,false,true ,false,fL,iL,jL,kL,nL);
  FsrEasuSetF(dir,len,pp,false,false,false,true ,gL,jL,kL,lL,oL);

  
  float2 dir2=dir*dir;
  float dirR=dir2.x+dir2.y;
  bool zro=dirR<float(1.0/32768.0);
  dirR=safeRsqrt(dirR);
  dirR=zro?float(1.0):dirR;
  dir.x=zro?float(1.0):dir.x;
  dir*=float2(dirR,dirR);
  
  len=len*float(0.5);
  len*=len;
  
  float stretch=(dir.x*dir.x+dir.y*dir.y)*safeRcp(max(abs(dir.x),abs(dir.y)));
  
  
  
  float2 len2=float2(float(1.0)+(stretch-float(1.0))*len,float(1.0)+float(-0.5)*len);
  
  
  float lob=float(0.5)+float((1.0/4.0-0.04)-0.5)*len;
  
  float clp=safeRcp(lob);

  
  
  
  
  
  float3 min4=min(min3(float3(ijfeR.z,ijfeG.z,ijfeB.z),float3(klhgR.w,klhgG.w,klhgB.w),float3(ijfeR.y,ijfeG.y,ijfeB.y)),
               float3(klhgR.x,klhgG.x,klhgB.x));
  float3 max4=max(max3(float3(ijfeR.z,ijfeG.z,ijfeB.z),float3(klhgR.w,klhgG.w,klhgB.w),float3(ijfeR.y,ijfeG.y,ijfeB.y)),
               float3(klhgR.x,klhgG.x,klhgB.x));
  
  float3 aC=float3(0.0,0.0,0.0);
  float aW=float(0.0);
  FsrEasuTapF(aC,aW,float2( 0.0,-1.0)-pp,dir,len2,lob,clp,float3(bczzR.x,bczzG.x,bczzB.x)); 
  FsrEasuTapF(aC,aW,float2( 1.0,-1.0)-pp,dir,len2,lob,clp,float3(bczzR.y,bczzG.y,bczzB.y)); 
  FsrEasuTapF(aC,aW,float2(-1.0, 1.0)-pp,dir,len2,lob,clp,float3(ijfeR.x,ijfeG.x,ijfeB.x)); 
  FsrEasuTapF(aC,aW,float2( 0.0, 1.0)-pp,dir,len2,lob,clp,float3(ijfeR.y,ijfeG.y,ijfeB.y)); 
  FsrEasuTapF(aC,aW,float2( 0.0, 0.0)-pp,dir,len2,lob,clp,float3(ijfeR.z,ijfeG.z,ijfeB.z)); 
  FsrEasuTapF(aC,aW,float2(-1.0, 0.0)-pp,dir,len2,lob,clp,float3(ijfeR.w,ijfeG.w,ijfeB.w)); 
  FsrEasuTapF(aC,aW,float2( 1.0, 1.0)-pp,dir,len2,lob,clp,float3(klhgR.x,klhgG.x,klhgB.x)); 
  FsrEasuTapF(aC,aW,float2( 2.0, 1.0)-pp,dir,len2,lob,clp,float3(klhgR.y,klhgG.y,klhgB.y)); 
  FsrEasuTapF(aC,aW,float2( 2.0, 0.0)-pp,dir,len2,lob,clp,float3(klhgR.z,klhgG.z,klhgB.z)); 
  FsrEasuTapF(aC,aW,float2( 1.0, 0.0)-pp,dir,len2,lob,clp,float3(klhgR.w,klhgG.w,klhgB.w)); 
  FsrEasuTapF(aC,aW,float2( 1.0, 2.0)-pp,dir,len2,lob,clp,float3(zzonR.z,zzonG.z,zzonB.z)); 
  FsrEasuTapF(aC,aW,float2( 0.0, 2.0)-pp,dir,len2,lob,clp,float3(zzonR.w,zzonG.w,zzonB.w)); 

  
  pix=min(max4,max(min4,aC*safeRcp(aW)));}

float3 Easu(float2 uv,float2 inputSize,float2 outputSize) {
    float2 ratio=inputSize/outputSize, t=1.0/inputSize;
    float4 c0=float4(ratio,0.5*ratio-0.5);
    float4 c1=float4(t,t.x,-t.y);
    float4 c2=float4(-t.x,2*t.y,t.x,2*t.y);
    float4 c3=float4(0,4*t.y,0,0);
    float3 result;
    FsrEasuF(result,uint2(uv*outputSize),c0,c1,c2,c3);
    return saturate(result);
}

float4 PSPresent(VertexOutput v) : SV_TARGET {
    uint iw,ih; plane0.GetDimensions(iw,ih);
    if (sharp.w > 0.5 && sharp.y > iw && sharp.z > ih)
        return float4(Easu(v.uv,float2(iw,ih),sharp.yz),1);
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

Texture2D<float4> history : register(t2);
Texture2D<float4> motion : register(t3);

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float2 findMotion(float2 uv, float2 texel, bool reverse) {
    float best = 1e9;
    float2 result = float2(0, 0);
    for (int stage = 0; stage < 2; ++stage) {
        float2 center = result;
        int radius = stage == 0 ? 8 : 3;
        float stride = stage == 0 ? 4.0 : 1.0;
        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                float2 d = center + float2(x, y) * stride * texel;
                float error = 0;
                for (int py = -1; py <= 1; ++py) {
                    for (int px = -1; px <= 1; ++px) {
                        float2 q = uv + float2(px, py) * texel * 4.0;
                        float a = luminance(reverse ? plane0.SampleLevel(linearSampler, q, 0).rgb : history.SampleLevel(linearSampler, q, 0).rgb);
                        float b = luminance(reverse ? history.SampleLevel(linearSampler, q + d, 0).rgb : plane0.SampleLevel(linearSampler, q + d, 0).rgb);
                        error += abs(a - b);
                    }
                }
                error += dot(d / texel, d / texel) * 0.000002;
                if (error < best) { best = error; result = d; }
            }
        }
    }
    return result;
}

float4 PSMotion(VertexOutput v) : SV_TARGET {
    uint w,h; plane0.GetDimensions(w,h);
    float2 t = 1.0 / float2(w,h);
    return float4(findMotion(v.uv,t,false), findMotion(v.uv,t,true));
}
float4 PSMidpoint(VertexOutput v) : SV_TARGET {
    float4 flow = motion.SampleLevel(linearSampler,v.uv,0);
    float2 aUV = v.uv - flow.xy * 0.5;
    float2 bUV = v.uv - flow.zw * 0.5;
    float3 a = history.SampleLevel(linearSampler,aUV,0).rgb;
    float3 b = plane0.SampleLevel(linearSampler,bUV,0).rgb;
    float3 current = plane0.SampleLevel(linearSampler,v.uv,0).rgb;
    // Reject mismatches, disocclusions and cuts locally rather than dissolving.
    float disagreement = max(abs(a.r-b.r),max(abs(a.g-b.g),abs(a.b-b.b)));
    bool inside = all(aUV >= 0) && all(aUV <= 1) && all(bUV >= 0) && all(bUV <= 1);
    float trust = inside ? 1.0-smoothstep(0.04,0.18,disagreement) : 0.0;
    return float4(lerp(current,(a+b)*0.5,trust),1);
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
        {"PSMotion", &psMotion_}, {"PSMidpoint", &psMidpoint_},
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
    previousRgb_.Reset(); previousView_.Reset();
    motionTexture_.Reset(); motionView_.Reset(); motionTarget_.Reset();
    midpointTexture_.Reset(); midpointView_.Reset(); midpointTarget_.Reset();
    cadence_.Reset(); showMidpoint_ = false;
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
        YuvParams presentParams = MakeYuvParams(textureBt709_, textureFullRange_);
        presentParams.sharp[0] = sharpness_;
        presentParams.sharp[1] = vw; presentParams.sharp[2] = vh;
        presentParams.sharp[3] = fsrUpscaling_ ? 1.0f : 0.0f;
        context_->UpdateSubresource(yuvConstants_.Get(), 0, nullptr, &presentParams, 0, 0);
        context_->PSSetShader(psPresent_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* views[] = {showMidpoint_ ? midpointView_.Get() : rgbView_.Get()};
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
    const auto step = cadence_.Next(frameGeneration_, f.fps, frame.sequence);
    showMidpoint_ = false;
    if (step == FrameGenerationCadence::Step::Endpoint) { Present(); return; }
    if (step == FrameGenerationCadence::Step::Midpoint) {
        if (!previousRgb_) {
            D3D11_TEXTURE2D_DESC desc{}; rgbTexture_->GetDesc(&desc);
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            Check(device_->CreateTexture2D(&desc, nullptr, &previousRgb_), "History texture");
            Check(device_->CreateShaderResourceView(previousRgb_.Get(), nullptr, &previousView_), "History view");
        }
        context_->ClearState();
        context_->CopyResource(previousRgb_.Get(), rgbTexture_.Get());
    }
    lastFormat_ = f;
    UpdateParams(f);
    if (CopyGpuFrame(frame)) {
        // Hardware-decoded: frame never touched system memory.
        Convert(gpuView0_.Get(), gpuView1_.Get());
    } else {
        texturePixel_ = f.pixel;
        if (!Upload(frame)) { cadence_.Reset(); return; }
        Convert(plane0View_.Get(), plane1View_.Get());
    }
    if (step == FrameGenerationCadence::Step::Midpoint) GenerateMidpoint();
    Present();
}

void D3DRenderer::GenerateMidpoint() {
    if (!motionTexture_) {
        auto make = [&](unsigned w, unsigned h, DXGI_FORMAT fmt,
                        ComPtr<ID3D11Texture2D>& texture,
                        ComPtr<ID3D11ShaderResourceView>& view,
                        ComPtr<ID3D11RenderTargetView>& target) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width=w; d.Height=h; d.MipLevels=1; d.ArraySize=1;
            d.Format=fmt; d.SampleDesc.Count=1;
            d.BindFlags=D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            Check(device_->CreateTexture2D(&d,nullptr,&texture), "Frame generation texture");
            Check(device_->CreateShaderResourceView(texture.Get(),nullptr,&view), "Frame generation SRV");
            Check(device_->CreateRenderTargetView(texture.Get(),nullptr,&target), "Frame generation RTV");
        };
        make((textureWidth_+15)/16,(textureHeight_+15)/16,DXGI_FORMAT_R32G32B32A32_FLOAT,
             motionTexture_,motionView_,motionTarget_);
        make(textureWidth_,textureHeight_,DXGI_FORMAT_B8G8R8A8_UNORM,
             midpointTexture_,midpointView_,midpointTarget_);
    }
    auto draw = [&](ID3D11RenderTargetView* target, ID3D11PixelShader* shader,
                    unsigned w, unsigned h, ID3D11ShaderResourceView* flow) {
        context_->ClearState();
        context_->OMSetRenderTargets(1,&target,nullptr);
        D3D11_VIEWPORT vp{0,0,static_cast<float>(w),static_cast<float>(h),0,1};
        context_->RSSetViewports(1,&vp);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->VSSetShader(vertexShader_.Get(),nullptr,0);
        context_->PSSetShader(shader,nullptr,0);
        ID3D11ShaderResourceView* views[] = {rgbView_.Get(),nullptr,previousView_.Get(),flow};
        context_->PSSetShaderResources(0,4,views);
        ID3D11SamplerState* sampler=sampler_.Get();
        context_->PSSetSamplers(0,1,&sampler);
        context_->Draw(3,0);
    };
    draw(motionTarget_.Get(),psMotion_.Get(),(textureWidth_+15)/16,(textureHeight_+15)/16,nullptr);
    draw(midpointTarget_.Get(),psMidpoint_.Get(),textureWidth_,textureHeight_,motionView_.Get());
    showMidpoint_ = true;
}

void D3DRenderer::Redraw() {
    if (hasImage_ && paramsDirty_) UpdateParams(lastFormat_);
    Present(); // also draws the UI over black when there is no video
}

#endif // _WIN32
