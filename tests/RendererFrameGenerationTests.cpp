#include "Renderer.h"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <chrono>

unsigned width=256,height=128;

struct D3DRendererTestAccess {
    static ID3D11Device* Device(D3DRenderer& r) { return r.device_.Get(); }
    static ID3D11DeviceContext* Context(D3DRenderer& r) { return r.context_.Get(); }
    static ID3D11Texture2D* Midpoint(D3DRenderer& r) { return r.midpointTexture_.Get(); }
    static bool Generated(D3DRenderer& r) { return r.showMidpoint_; }
};
void check(bool value,const char* text) { if (!value) throw std::runtime_error(text); }
unsigned char pattern(int x,int y,int channel) {
    const float v=0.5f+0.22f*std::sin(x*0.19f+channel)+0.18f*std::cos(y*0.27f+x*0.07f+channel);
    return static_cast<unsigned char>(v*255);
}
VideoFrame frame(unsigned sequence,int shift) {
    VideoFrame f;
    f.format.width=width; f.format.height=height; f.format.stride=width*4;
    f.format.fps=60; f.format.pixel=PixelFormat::BGRA; f.sequence=sequence;
    check(SUCCEEDED(MFCreateMemoryBuffer(width*height*4,&f.buffer)),"buffer");
    BYTE* bytes=nullptr; DWORD max=0,len=0;
    check(SUCCEEDED(f.buffer->Lock(&bytes,&max,&len)),"lock");
    for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
        for(int c=0;c<3;++c) bytes[(y*width+x)*4+c]=pattern(int(x)-shift,int(y),c);
        bytes[(y*width+x)*4+3]=255;
    }
    f.buffer->Unlock(); f.buffer->SetCurrentLength(width*height*4);
    return f;
}
int main(int argc,char**) {
    if (argc>1) { width=1920; height=1080; }
    CoInitializeEx(nullptr,COINIT_MULTITHREADED); MFStartup(MF_VERSION);
    HWND window=CreateWindowExW(0,L"STATIC",L"DirectView render test",WS_OVERLAPPEDWINDOW,
        0,0,512,256,nullptr,nullptr,GetModuleHandle(nullptr),nullptr);
    int result=0;
    try {
        D3DRenderer r; r.Initialize(window); r.SetVSync(false); r.SetFrameGeneration(true);
        r.Render(frame(1,0)); r.Render(frame(2,0));
        auto sample=frame(3,8);
        const auto started=std::chrono::steady_clock::now();
        r.Render(sample);
        check(D3DRendererTestAccess::Generated(r),"midpoint was not generated");
        auto* image=D3DRendererTestAccess::Midpoint(r);
        D3D11_TEXTURE2D_DESC d{}; image->GetDesc(&d);
        d.Usage=D3D11_USAGE_STAGING; d.BindFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> read;
        check(SUCCEEDED(D3DRendererTestAccess::Device(r)->CreateTexture2D(&d,nullptr,&read)),"readback texture");
        auto* ctx=D3DRendererTestAccess::Context(r);
        ctx->ClearState(); ctx->CopyResource(read.Get(),image);
        D3D11_MAPPED_SUBRESOURCE m{};
        check(SUCCEEDED(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&m)),"readback map");
        double generatedError=0,endpointError=0; int count=0;
        for(int y=24;y<104;++y) for(int x=40;x<216;++x) for(int c=0;c<3;++c) {
            const int expected=pattern(x-4,y,c);
            const auto* row=static_cast<const BYTE*>(m.pData)+y*m.RowPitch;
            generatedError+=std::abs(int(row[x*4+c])-expected);
            endpointError+=std::abs(int(pattern(x-8,y,c))-expected); ++count;
        }
        ctx->Unmap(read.Get(),0);
        printf("%ux%u first midpoint render + synchronous readback: %.2f ms (includes allocation)\n",
            width,height,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count());
        printf("Generated midpoint MAE %.3f vs endpoint MAE %.3f (8-bit levels)\n",generatedError/count,endpointError/count);
        check(generatedError<endpointError*0.65,"motion midpoint does not improve position accuracy");
        r.Render(frame(4,8)); check(!D3DRendererTestAccess::Generated(r),"duplicate must present real endpoint");
        r.Render(frame(7,16)); check(!D3DRendererTestAccess::Generated(r),"gap must reseed");
        r.SetFsrUpscaling(true); r.Resize(512,256); r.Redraw();
        r.SetFrameGeneration(false); r.Render(frame(8,16));
        check(!D3DRendererTestAccess::Generated(r),"disabled mode generated a frame");
        puts("D3D11 motion interpolation and FSR presentation smoke tests passed");
    } catch(const std::exception& e) { fprintf(stderr,"FAIL: %s\n",e.what()); result=1; }
    DestroyWindow(window); MFShutdown(); CoUninitialize(); return result;
}
