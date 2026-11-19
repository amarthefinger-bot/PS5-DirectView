#include "../Renderer.h"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreVideo/CVMetalTextureCache.h>

#include <cmath>
#include <cstdint>

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

fragment float4 fs_present(VsOut in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           constant YuvParams& p [[buffer(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
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
    CVMetalTextureCacheRef textureCache = NULL;

    id<MTLTexture> rgbTexture = nil;        // offscreen converted image at source res
    MTLTextureDescriptor* rgbDesc = nil;
    PixelFormat lastPixel = PixelFormat::BGRA;
    bool lastFullRange = false;
    bool lastBt709 = true;
    unsigned lastW = 0, lastH = 0;

    D3DRenderer* owner = nullptr;

    bool Build(NSError** outError) {
        device = MTLCreateSystemDefaultDevice();
        if (!device) return false;
        queue = [device newCommandQueue];
        id<MTLLibrary> lib = [device newLibraryWithSource:kMetalShaders
                                                  options:nil
                                                    error:outError];
        if (!lib) return false;

        auto makePso = ^id<MTLRenderPipelineState>(NSString* vs, NSString* fs) {
            MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
            d.vertexFunction = [lib newFunctionWithName:vs];
            d.fragmentFunction = [lib newFunctionWithName:fs];
            d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            NSError* err = nil;
            id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:d error:&err];
            return pso;
        };
        psoBgra = makePso(@"vs_fullscreen", @"fs_bgra");
        psoYuy2 = makePso(@"vs_fullscreen", @"fs_yuy2");
        psoNv12 = makePso(@"vs_fullscreen", @"fs_nv12");
        psoPresent = makePso(@"vs_fullscreen", @"fs_present");
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
    if (!impl_) return;
    impl_->view.drawableSize = CGSizeMake(width, height);
}

void D3DRenderer::ResetVideo() {
    if (!impl_) return;
    impl_->rgbTexture = nil;
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

        id<MTLCommandBuffer> cmd = [impl_->queue commandBuffer];

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

        // ---- Pass 2: present with optional CAS sharpen.
        id<CAMetalDrawable> drawable = impl_->view.currentDrawable;
        if (drawable) {
            MTLRenderPassDescriptor* prp = [[MTLRenderPassDescriptor alloc] init];
            prp.colorAttachments[0].texture = drawable.texture;
            prp.colorAttachments[0].loadAction = MTLLoadActionClear;
            prp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            prp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> penc = [cmd renderCommandEncoderWithDescriptor:prp];
            [penc setRenderPipelineState:impl_->psoPresent];
            [penc setFragmentTexture:impl_->rgbTexture atIndex:0];
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
        id<MTLCommandBuffer> cmd = [impl_->queue commandBuffer];
        MTLRenderPassDescriptor* prp = [[MTLRenderPassDescriptor alloc] init];
        prp.colorAttachments[0].texture = drawable.texture;
        prp.colorAttachments[0].loadAction = MTLLoadActionClear;
        prp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        prp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:prp];
        [enc setRenderPipelineState:impl_->psoPresent];
        [enc setFragmentTexture:impl_->rgbTexture atIndex:0];
        [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cmd presentDrawable:drawable];
        [cmd commit];
    }
}

D3DRenderer::D3DRenderer() = default;
D3DRenderer::~D3DRenderer() = default;
