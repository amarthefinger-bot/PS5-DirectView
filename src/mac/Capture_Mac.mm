#include "../Capture.h"

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMediaIO/CMIOHardware.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>

VideoFrame::~VideoFrame() { Reset(); }

VideoFrame::VideoFrame(VideoFrame&& other) noexcept {
    pixelBuffer = other.pixelBuffer;
    format = other.format;
    sampleTime = other.sampleTime;
    receiveTime = other.receiveTime;
    sequence = other.sequence;
    other.pixelBuffer = nullptr;
}

VideoFrame& VideoFrame::operator=(VideoFrame&& other) noexcept {
    if (this != &other) {
        Reset();
        pixelBuffer = other.pixelBuffer;
        format = other.format;
        sampleTime = other.sampleTime;
        receiveTime = other.receiveTime;
        sequence = other.sequence;
        other.pixelBuffer = nullptr;
    }
    return *this;
}

void VideoFrame::Reset() {
    if (pixelBuffer) {
        CVPixelBufferRelease(static_cast<CVPixelBufferRef>(pixelBuffer));
        pixelBuffer = nullptr;
    }
}

void LatestFrame::Publish(VideoFrame&& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame.sequence = ++nextSequence_;
    latest_ = std::move(frame);
}

bool LatestFrame::Consume(VideoFrame& destination) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_.pixelBuffer) return false;
    destination = std::move(latest_);
    latest_ = VideoFrame{};
    return true;
}

void LatestFrame::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = VideoFrame{};
    nextSequence_ = 0;
}

@interface DirectViewCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (nonatomic, assign) CaptureService::Impl* impl;
@end

struct CaptureService::Impl {
    CaptureService* owner = nullptr;
    AVCaptureSession* session = nil;
    AVCaptureDevice* device = nil;
    AVCaptureDeviceInput* input = nil;
    AVCaptureVideoDataOutput* output = nil;
    DirectViewCaptureDelegate* delegate = nil;
    dispatch_queue_t queue = nullptr;
    std::atomic<std::uint64_t> framesReceived{0};

    void HandleSample(CMSampleBufferRef sample) {
        CVImageBufferRef pb = CMSampleBufferGetImageBuffer(sample);
        if (!pb) return;
        VideoFrame frame;
        frame.pixelBuffer = CVPixelBufferRetain(pb);

        const OSType type = CVPixelBufferGetPixelFormatType(pb);
        FrameFormat fmt;
        switch (type) {
            case kCVPixelFormatType_32BGRA:
                fmt.pixel = PixelFormat::BGRA; break;
            case kCVPixelFormatType_422YpCbCr8: // 2vuy
            case kCVPixelFormatType_422YpCbCr8_yuvs:
                fmt.pixel = PixelFormat::YUY2; break;
            case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
            case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
                fmt.pixel = PixelFormat::NV12;
                fmt.fullRange = (type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
                break;
            default:
                // Unsupported; drop frame.
                return;
        }
        fmt.width = (unsigned)CVPixelBufferGetWidth(pb);
        fmt.height = (unsigned)CVPixelBufferGetHeight(pb);
        fmt.codedHeight = fmt.height;
        fmt.stride = (long)CVPixelBufferGetBytesPerRow(pb);
        CMTime dur = CMSampleBufferGetDuration(sample);
        if (dur.timescale > 0 && dur.value > 0)
            fmt.fps = (double)dur.timescale / (double)dur.value;
        fmt.bt709 = fmt.height >= 720;
        frame.format = fmt;

        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        frame.sampleTime = (pts.timescale > 0)
            ? (std::uint64_t)((double)pts.value / (double)pts.timescale * 1e9)
            : 0;
        auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        frame.receiveTime = (std::uint64_t)nowNs;

        owner->latest_.Publish(std::move(frame));
        ++framesReceived;
        owner->framesReceived_.store(framesReceived.load());

        char s[128];
        snprintf(s, sizeof(s), "%ux%u \xc2\xb7 %.0f fps \xc2\xb7 %s",
                 fmt.width, fmt.height, fmt.fps,
                 (fmt.pixel == PixelFormat::BGRA ? "BGRA" :
                  fmt.pixel == PixelFormat::YUY2 ? "YUY2" : "NV12"));
        {
            std::lock_guard<std::mutex> lock(owner->statusMutex_);
            owner->signal_.assign(s);
        }
        if (owner->frameEvent_) owner->frameEvent_->Signal();
    }
};

@implementation DirectViewCaptureDelegate
- (void)captureOutput:(AVCaptureOutput*)output
  didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
         fromConnection:(AVCaptureConnection*)connection {
    (void)output; (void)connection;
    if (self.impl) self.impl->HandleSample(sampleBuffer);
}
@end

std::vector<VideoDevice> EnumerateVideoDevices() {
    std::vector<VideoDevice> out;

    // Allow DAL plug-ins (capture-card vendor drivers, Elgato, Magewell, ...).
    CMIOObjectPropertyAddress addr = {
        kCMIOHardwarePropertyAllowScreenCaptureDevices,
        kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain };
    UInt32 allow = 1;
    CMIOObjectSetPropertyData(kCMIOObjectSystemObject, &addr, 0, nullptr,
                              sizeof(allow), &allow);

    AVCaptureDeviceDiscoverySession* d = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[
            AVCaptureDeviceTypeExternalUnknown,
            AVCaptureDeviceTypeBuiltInWideAngleCamera ]
        mediaType:AVMediaTypeVideo
        position:AVCaptureDevicePositionUnspecified];
    for (AVCaptureDevice* dev in d.devices) {
        VideoDevice entry;
        entry.name = dev.localizedName.UTF8String ?: "";
        entry.symbolicLink = dev.uniqueID.UTF8String ?: "";
        out.push_back(std::move(entry));
    }
    return out;
}

CaptureService::CaptureService(FrameEvent* frameEvent)
    : impl_(std::make_unique<Impl>()), frameEvent_(frameEvent) {
    impl_->owner = this;
    impl_->queue = dispatch_queue_create("dev.ps5directview.capture",
                                          DISPATCH_QUEUE_SERIAL);
    dispatch_set_target_queue(impl_->queue,
        dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0));
}

CaptureService::~CaptureService() { Stop(); }

std::string CaptureService::Status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

std::string CaptureService::Signal() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return signal_;
}

void CaptureService::Start(std::string deviceUniqueID) {
    Stop();

    AVCaptureDevice* dev = [AVCaptureDevice deviceWithUniqueID:
        [NSString stringWithUTF8String:deviceUniqueID.c_str()]];
    if (!dev) {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = "Capture device not found: " + deviceUniqueID;
        return;
    }

    NSError* err = nil;
    AVCaptureDeviceInput* in = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
    if (!in) {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = std::string("AVCaptureDeviceInput failed: ") + err.localizedDescription.UTF8String;
        return;
    }

    AVCaptureSession* sess = [[AVCaptureSession alloc] init];
    if ([sess canSetSessionPreset:AVCaptureSessionPresetHigh])
        sess.sessionPreset = AVCaptureSessionPresetHigh;
    [sess addInput:in];

    AVCaptureVideoDataOutput* out = [[AVCaptureVideoDataOutput alloc] init];
    out.alwaysDiscardsLateVideoFrames = YES;
    // Prefer NV12 (hardware-decoded path) then YUY2 then BGRA.
    out.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey:
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
    };

    DirectViewCaptureDelegate* del = [DirectViewCaptureDelegate new];
    del.impl = impl_.get();
    [out setSampleBufferDelegate:del queue:impl_->queue];
    [sess addOutput:out];

    impl_->device = dev;
    impl_->input = in;
    impl_->output = out;
    impl_->delegate = del;
    impl_->session = sess;

    [sess startRunning];
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = std::string("Capturing ") + dev.localizedName.UTF8String;
    }
}

void CaptureService::Stop() {
    if (!impl_->session) {
        running_ = false;
        return;
    }
    [impl_->session stopRunning];
    for (AVCaptureInput* in in impl_->session.inputs)
        [impl_->session removeInput:in];
    for (AVCaptureOutput* out in impl_->session.outputs)
        [impl_->session removeOutput:out];
    impl_->session = nil;
    impl_->input = nil;
    impl_->output = nil;
    impl_->delegate.impl = nullptr;
    impl_->delegate = nil;
    impl_->device = nil;
    latest_.Clear();
    running_ = false;
    framesReceived_ = 0;
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        status_ = "Capture stopped.";
        signal_.clear();
    }
}
