#include "../Renderer.h"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CVMetalTextureCache.h>

#include <cmath>
#include <cstdint>
#include <algorithm>

static NSString* const kMetalShaders = @R"METAL(
#include <metal_stdlib>
using namespace metal;

struct VsOut {
    float4 position [[position]];
    float2 uv;
};

vertex VsOut vs_fullscreen(uint vid [[vertex_id]]) {
    VsOut v;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    v.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    v.uv = uv;
    return v;
}

struct YuvParams {
    float4 range; // x: luma scale, y: luma offset, z: chroma scale, w: chroma offset
    float4 coeff; // x: R<-V, y: G<-U, z: G<-V, w: B<-U
    float4 sharp; // x: amount 0..1
};

static float3 yuv_to_rgb(float y, float u, float v, constant YuvParams& p) {
    y = (y - p.range.y) * p.range.x;
    u = (u - p.range.w) * p.range.z;
    v = (v - p.range.w) * p.range.z;
    return saturate(float3(y + p.coeff.x * v,
                           y + p.coeff.y * u + p.coeff.z * v,
                           y + p.coeff.w * u));
}

fragment float4 fs_bgra(VsOut in [[stage_in]],
                        texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return float4(tex.sample(s, in.uv).rgb, 1.0);
}

fragment float4 fs_yuy2(VsOut in [[stage_in]],
                        texture2d<float> tex [[texture(0)]],
                        constant YuvParams& p [[buffer(0)]]) {
    uint w = tex.get_width(), h = tex.get_height();
    uint2 px = uint2(in.uv.x * (float)(w * 2), in.uv.y * (float)h);
    float4 t = tex.read(uint2(px.x >> 1, px.y));
    float y = (px.x & 1u) ? t.b : t.r;
    return float4(yuv_to_rgb(y, t.g, t.a, p), 1.0);
}

fragment float4 fs_nv12(VsOut in [[stage_in]],
                        texture2d<float> luma [[texture(0)]],
                        texture2d<float> chroma [[texture(1)]],
                        constant YuvParams& p [[buffer(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float y = luma.sample(s, in.uv).r;
    float2 c = chroma.sample(s, in.uv).rg;
    return float4(yuv_to_rgb(y, c.x, c.y, p), 1.0);
}


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

float4 FsrEasuRF(float2 p,texture2d<float> tex) { constexpr sampler s(filter::linear,address::clamp_to_edge); return tex.gather(s,p,component::x); }
float4 FsrEasuGF(float2 p,texture2d<float> tex) { constexpr sampler s(filter::linear,address::clamp_to_edge); return tex.gather(s,p,component::y); }
float4 FsrEasuBF(float2 p,texture2d<float> tex) { constexpr sampler s(filter::linear,address::clamp_to_edge); return tex.gather(s,p,component::z); }
 void FsrEasuTapF(
 thread float3& aC, 
 thread float& aW, 
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
 thread float2& dir,
 thread float& len,
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
 thread float3& pix,
 uint2 ip, 
 float4 con0, 
 float4 con1,
 float4 con2,
 float4 con3, texture2d<float> tex){

  
  float2 pp=float2(ip)*con0.xy+con0.zw;
  float2 fp=floor(pp);
  pp-=fp;

  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  
  float2 p0=fp*con1.xy+con1.zw;
  
  float2 p1=p0+con2.xy;
  float2 p2=p0+con2.zw;
  float2 p3=p0+con3.xy;
  float4 bczzR=FsrEasuRF(p0,tex);
  float4 bczzG=FsrEasuGF(p0,tex);
  float4 bczzB=FsrEasuBF(p0,tex);
  float4 ijfeR=FsrEasuRF(p1,tex);
  float4 ijfeG=FsrEasuGF(p1,tex);
  float4 ijfeB=FsrEasuBF(p1,tex);
  float4 klhgR=FsrEasuRF(p2,tex);
  float4 klhgG=FsrEasuGF(p2,tex);
  float4 klhgB=FsrEasuBF(p2,tex);
  float4 zzonR=FsrEasuRF(p3,tex);
  float4 zzonG=FsrEasuGF(p3,tex);
  float4 zzonB=FsrEasuBF(p3,tex);

  
  float4 bczzL=bczzB*float4(0.5)+(bczzR*float4(0.5)+bczzG);
  float4 ijfeL=ijfeB*float4(0.5)+(ijfeR*float4(0.5)+ijfeG);
  float4 klhgL=klhgB*float4(0.5)+(klhgR*float4(0.5)+klhgG);
  float4 zzonL=zzonB*float4(0.5)+(zzonR*float4(0.5)+zzonG);
  
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
  
  float2 dir=float2(0.0);
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
  dir*=float2(dirR);
  
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
  
  float3 aC=float3(0.0);
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

  
  pix=min(max4,max(min4,aC*float3(safeRcp(aW))));}

float3 Easu(float2 uv,float2 inputSize,float2 outputSize,texture2d<float> tex) {
    float2 ratio=inputSize/outputSize, t=1.0/inputSize;
    float4 c0=float4(ratio,0.5*ratio-0.5);
    float4 c1=float4(t,t.x,-t.y);
    float4 c2=float4(-t.x,2*t.y,t.x,2*t.y);
    float4 c3=float4(0,4*t.y,0,0);
    float3 result;
    FsrEasuF(result,uint2(uv*outputSize),c0,c1,c2,c3,tex);
    return saturate(result);
}

fragment float4 fs_present(VsOut in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           constant YuvParams& p [[buffer(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    if (p.sharp.w > 0.5 && p.sharp.y > tex.get_width() && p.sharp.z > tex.get_height())
        return float4(Easu(in.uv,float2(tex.get_width(),tex.get_height()),p.sharp.yz,tex),1);
    float3 c = tex.sample(s, in.uv).rgb;
    if (p.sharp.x <= 0.0) return float4(c, 1.0);
    uint w = tex.get_width(), h = tex.get_height();
    float2 t = float2(1.0 / (float)w, 1.0 / (float)h);
    float3 n = tex.sample(s, in.uv + float2(0, -t.y)).rgb;
    float3 S = tex.sample(s, in.uv + float2(0,  t.y)).rgb;
    float3 e = tex.sample(s, in.uv + float2( t.x, 0)).rgb;
    float3 o = tex.sample(s, in.uv + float2(-t.x, 0)).rgb;
    float3 mn = min(c, min(min(n, S), min(e, o)));
    float3 mx = max(c, max(max(n, S), max(e, o)));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-4)));
    float3 wgt = amp * (-1.0 / mix(8.0, 5.0, p.sharp.x));
    float3 r = (c + (n + S + e + o) * wgt) / (1.0 + 4.0 * wgt);
    return float4(saturate(r), 1.0);
}

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float2 findMotion(float2 uv, float2 texel, bool reverse, texture2d<float> cur, texture2d<float> old) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
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
                        float a = luminance(reverse ? cur.sample(s,q).rgb : old.sample(s,q).rgb);
                        float b = luminance(reverse ? old.sample(s,q+d).rgb : cur.sample(s,q+d).rgb);
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

fragment float4 fs_motion(VsOut in [[stage_in]], texture2d<float> cur [[texture(0)]],
                          texture2d<float> old [[texture(1)]]) {
    float2 t = 1.0 / float2(cur.get_width(),cur.get_height());
    return float4(findMotion(in.uv,t,false,cur,old),findMotion(in.uv,t,true,cur,old));
}
fragment float4 fs_midpoint(VsOut in [[stage_in]], texture2d<float> cur [[texture(0)]],
                            texture2d<float> old [[texture(1)]], texture2d<float> motion [[texture(2)]]) {
    constexpr sampler s(filter::linear,address::clamp_to_edge);
    float4 flow=motion.sample(s,in.uv);
    float2 aUV=in.uv-flow.xy*0.5, bUV=in.uv-flow.zw*0.5;
    float3 a=old.sample(s,aUV).rgb, b=cur.sample(s,bUV).rgb;
    float3 current=cur.sample(s,in.uv).rgb;
    float disagreement=max(abs(a.r-b.r),max(abs(a.g-b.g),abs(a.b-b.b)));
    bool inside=all(aUV>=0) && all(aUV<=1) && all(bUV>=0) && all(bUV<=1);
    float trust=inside ? 1.0-smoothstep(0.04,0.18,disagreement) : 0.0;
    return float4(mix(current,(a+b)*0.5,trust),1);
}

)METAL";

namespace {

struct YuvParams {
    float range[4];
    float coeff[4];
    float sharp[4];
};

YuvParams MakeYuvParams(bool bt709, bool fullRange, float sharpness) {
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
    p.sharp[0] = sharpness;
    return p;
}

} // namespace

struct D3DRenderer::Impl {
    MTKView* view = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> psoBgra = nil;
    id<MTLRenderPipelineState> psoYuy2 = nil;
    id<MTLRenderPipelineState> psoNv12 = nil;
    id<MTLRenderPipelineState> psoPresent = nil;
    id<MTLRenderPipelineState> psoMotion = nil, psoMidpoint = nil;
    id<MTLTexture> previousRgb = nil, motionTexture = nil, midpointTexture = nil;
    bool showMidpoint = false;
    CVMetalTextureCacheRef textureCache = NULL;

    id<MTLTexture> rgbTexture = nil;        // offscreen converted image at source res
    MTLTextureDescriptor* rgbDesc = nil;
    PixelFormat lastPixel = PixelFormat::BGRA;
    bool lastFullRange = false;
    bool lastBt709 = true;
    unsigned lastW = 0, lastH = 0;

    D3DRenderer* owner = nullptr;

    void SetPresentViewport(id<MTLRenderCommandEncoder> encoder,
                            id<CAMetalDrawable> drawable, YuvParams& params) {
        const double width = drawable.texture.width, height = drawable.texture.height;
        const double scale = std::min(width / lastW, height / lastH);
        const double vw = lastW * scale, vh = lastH * scale;
        MTLViewport viewport = {(width-vw)*0.5, (height-vh)*0.5, vw, vh, 0, 1};
        [encoder setViewport:viewport];
        params.sharp[1] = static_cast<float>(vw);
        params.sharp[2] = static_cast<float>(vh);
    }

    bool Build(NSError** outError) {
        device = MTLCreateSystemDefaultDevice();
        if (!device) return false;
        queue = [device newCommandQueue];
        id<MTLLibrary> lib = [device newLibraryWithSource:kMetalShaders
                                                  options:nil
                                                    error:outError];
        if (!lib) return false;

        auto makePso = ^id<MTLRenderPipelineState>(NSString* vs, NSString* fs, MTLPixelFormat format) {
            MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
            d.vertexFunction = [lib newFunctionWithName:vs];
            d.fragmentFunction = [lib newFunctionWithName:fs];
            d.colorAttachments[0].pixelFormat = format;
            NSError* err = nil;
            id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:d error:&err];
            return pso;
        };
        psoBgra = makePso(@"vs_fullscreen", @"fs_bgra", MTLPixelFormatBGRA8Unorm);
        psoYuy2 = makePso(@"vs_fullscreen", @"fs_yuy2", MTLPixelFormatBGRA8Unorm);
        psoNv12 = makePso(@"vs_fullscreen", @"fs_nv12", MTLPixelFormatBGRA8Unorm);
        psoPresent = makePso(@"vs_fullscreen", @"fs_present", MTLPixelFormatBGRA8Unorm);
        psoMotion = makePso(@"vs_fullscreen", @"fs_motion", MTLPixelFormatRGBA32Float);
        psoMidpoint = makePso(@"vs_fullscreen", @"fs_midpoint", MTLPixelFormatBGRA8Unorm);
        if (!psoMotion || !psoMidpoint) return false;
        if (!psoBgra || !psoYuy2 || !psoNv12 || !psoPresent) return false;

        return CVMetalTextureCacheCreate(kCFAllocatorDefault, NULL, device, NULL,
                                         &textureCache) == kCVReturnSuccess;
    }

    void EnsureRgbTexture(unsigned w, unsigned h) {
        if (rgbTexture && lastW == w && lastH == h) return;
        rgbDesc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:w
                                        height:h
                                     mipmapped:NO];
        rgbDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        rgbDesc.storageMode = MTLStorageModePrivate;
        rgbTexture = [device newTextureWithDescriptor:rgbDesc];
        previousRgb = nil; motionTexture = nil; midpointTexture = nil;
        owner->cadence_.Reset(); showMidpoint = false;
        lastW = w; lastH = h;
    }

    id<MTLTexture> TextureFromPixelBuffer(CVPixelBufferRef pb,
                                          MTLPixelFormat format,
                                          size_t plane,
                                          size_t width, size_t height) {
        CVMetalTextureRef mt = NULL;
        CVReturn r = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, textureCache, pb, NULL, format,
            width, height, plane, &mt);
        if (r != kCVReturnSuccess || !mt) return nil;
        id<MTLTexture> tex = CVMetalTextureGetTexture(mt);
        CFRelease(mt);
        return tex;
    }
};

void D3DRenderer::Initialize(void* metalView) {
    impl_ = std::make_unique<Impl>();
    impl_->owner = this;
    impl_->view = (__bridge MTKView*)metalView;
    NSError* err = nil;
    if (!impl_->Build(&err)) {
        NSLog(@"Metal init failed: %@", err);
        return;
    }
    impl_->view.device = impl_->device;
    impl_->view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    impl_->view.framebufferOnly = YES;
    impl_->view.enableSetNeedsDisplay = NO;
    impl_->view.paused = NO;
}

void D3DRenderer::Resize(unsigned width, unsigned height) {
    // MTKView.autoResizeDrawable owns its drawable size. Keep the shared API
    // notification-only so no caller can feed a resize back into MetalKit.
    (void)width;
    (void)height;
}

void D3DRenderer::ResetVideo() {
    if (!impl_) return;
    impl_->rgbTexture = nil;
    impl_->previousRgb = nil; impl_->motionTexture = nil; impl_->midpointTexture = nil;
    impl_->showMidpoint = false; cadence_.Reset();
    impl_->lastW = impl_->lastH = 0;
    hasImage_ = false;
}

void D3DRenderer::Render(const VideoFrame& frame) {
    if (!impl_ || !frame.pixelBuffer) return;
    CVPixelBufferRef pb = static_cast<CVPixelBufferRef>(frame.pixelBuffer);
    const FrameFormat& fmt = frame.format;
    const bool fullRange = (rangeOverride_ == 1) ? true
                         : (rangeOverride_ == 2) ? false : fmt.fullRange;
    YuvParams params = MakeYuvParams(fmt.bt709, fullRange, sharpness_);

    @autoreleasepool {
        impl_->EnsureRgbTexture(fmt.width, fmt.height);

        const auto step = cadence_.Next(frameGeneration_, fmt.fps, frame.sequence);
        impl_->showMidpoint = false;
        if (step == FrameGenerationCadence::Step::Endpoint) { Redraw(); return; }
        id<MTLCommandBuffer> cmd = [impl_->queue commandBuffer];
        if (step == FrameGenerationCadence::Step::Midpoint) {
            if (!impl_->previousRgb) {
                impl_->previousRgb = [impl_->device newTextureWithDescriptor:impl_->rgbDesc];
                impl_->midpointTexture = [impl_->device newTextureWithDescriptor:impl_->rgbDesc];
                MTLTextureDescriptor* md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                    width:(fmt.width+15)/16 height:(fmt.height+15)/16 mipmapped:NO];
                md.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                md.storageMode = MTLStorageModePrivate;
                impl_->motionTexture = [impl_->device newTextureWithDescriptor:md];
            }
            id<MTLBlitCommandEncoder> copy = [cmd blitCommandEncoder];
            [copy copyFromTexture:impl_->rgbTexture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(fmt.width,fmt.height,1) toTexture:impl_->previousRgb
                destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
            [copy endEncoding];
        }

        // ---- Pass 1: convert to RGB into offscreen texture.
        MTLRenderPassDescriptor* rp = [[MTLRenderPassDescriptor alloc] init];
        rp.colorAttachments[0].texture = impl_->rgbTexture;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];

        if (fmt.pixel == PixelFormat::NV12) {
            id<MTLTexture> yTex = impl_->TextureFromPixelBuffer(pb, MTLPixelFormatR8Unorm,
                                                                 0, fmt.width, fmt.height);
            id<MTLTexture> cTex = impl_->TextureFromPixelBuffer(pb, MTLPixelFormatRG8Unorm,
                                                                 1, fmt.width / 2, fmt.height / 2);
            [enc setRenderPipelineState:impl_->psoNv12];
            if (yTex) [enc setFragmentTexture:yTex atIndex:0];
            if (cTex) [enc setFragmentTexture:cTex atIndex:1];
            [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
        } else if (fmt.pixel == PixelFormat::YUY2) {
            // YUY2 is 16 bpp; sample as RGBA8 over half the width.
            id<MTLTexture> tex = impl_->TextureFromPixelBuffer(pb, MTLPixelFormatRGBA8Unorm,
                                                                0, fmt.width / 2, fmt.height);
            [enc setRenderPipelineState:impl_->psoYuy2];
            if (tex) [enc setFragmentTexture:tex atIndex:0];
            [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
        } else {
            id<MTLTexture> tex = impl_->TextureFromPixelBuffer(pb, MTLPixelFormatBGRA8Unorm,
                                                                0, fmt.width, fmt.height);
            [enc setRenderPipelineState:impl_->psoBgra];
            if (tex) [enc setFragmentTexture:tex atIndex:0];
        }
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];

        if (step == FrameGenerationCadence::Step::Midpoint) {
            auto draw = [&](id<MTLTexture> target, id<MTLRenderPipelineState> pipeline, bool useFlow) {
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [cmd renderCommandEncoderWithDescriptor:pass];
                [e setRenderPipelineState:pipeline];
                [e setFragmentTexture:impl_->rgbTexture atIndex:0];
                [e setFragmentTexture:impl_->previousRgb atIndex:1];
                if (useFlow) [e setFragmentTexture:impl_->motionTexture atIndex:2];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
            };
            draw(impl_->motionTexture,impl_->psoMotion,false);
            draw(impl_->midpointTexture,impl_->psoMidpoint,true);
            impl_->showMidpoint = true;
        }

        // ---- Pass 2: present with optional CAS sharpen.
        id<CAMetalDrawable> drawable = impl_->view.currentDrawable;
        if (drawable) {
            params.sharp[1] = drawable.texture.width;
            params.sharp[2] = drawable.texture.height;
            params.sharp[3] = fsrUpscaling_ ? 1.0f : 0.0f;
            MTLRenderPassDescriptor* prp = [[MTLRenderPassDescriptor alloc] init];
            prp.colorAttachments[0].texture = drawable.texture;
            prp.colorAttachments[0].loadAction = MTLLoadActionClear;
            prp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            prp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> penc = [cmd renderCommandEncoderWithDescriptor:prp];
            impl_->SetPresentViewport(penc, drawable, params);
            [penc setRenderPipelineState:impl_->psoPresent];
            [penc setFragmentTexture:(impl_->showMidpoint ? impl_->midpointTexture : impl_->rgbTexture) atIndex:0];
            [penc setFragmentBytes:&params length:sizeof(params) atIndex:0];
            [penc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [penc endEncoding];
            [cmd presentDrawable:drawable];
        }

        [cmd commit];
        CVMetalTextureCacheFlush(impl_->textureCache, 0);
    }

    hasImage_ = true;
}

void D3DRenderer::Redraw() {
    if (!impl_ || !hasImage_ || !impl_->rgbTexture) return;
    @autoreleasepool {
        id<CAMetalDrawable> drawable = impl_->view.currentDrawable;
        if (!drawable) return;
        YuvParams params = MakeYuvParams(true, false, sharpness_);
        params.sharp[1] = drawable.texture.width;
        params.sharp[2] = drawable.texture.height;
        params.sharp[3] = fsrUpscaling_ ? 1.0f : 0.0f;
        id<MTLCommandBuffer> cmd = [impl_->queue commandBuffer];
        MTLRenderPassDescriptor* prp = [[MTLRenderPassDescriptor alloc] init];
        prp.colorAttachments[0].texture = drawable.texture;
        prp.colorAttachments[0].loadAction = MTLLoadActionClear;
        prp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        prp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:prp];
        impl_->SetPresentViewport(enc, drawable, params);
        [enc setRenderPipelineState:impl_->psoPresent];
        [enc setFragmentTexture:(impl_->showMidpoint ? impl_->midpointTexture : impl_->rgbTexture) atIndex:0];
        [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cmd presentDrawable:drawable];
        [cmd commit];
    }
}

D3DRenderer::D3DRenderer() = default;
D3DRenderer::~D3DRenderer() = default;
