#include "../Audio.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <vector>

namespace {
constexpr UInt32 kRingFrames = 48000;  // 1 s at 48 kHz, stereo
} // namespace

@interface DirectViewAudioDelegate : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate>
@property (nonatomic, assign) AudioPassthrough::Impl* impl;
@end

struct AudioPassthrough::Impl {
    AVCaptureSession* session = nil;
    DirectViewAudioDelegate* delegate = nil;
    dispatch_queue_t queue = nullptr;

    AudioUnit outputUnit = nullptr;
    AudioStreamBasicDescription outputFormat{};

    std::mutex ringMutex;
    std::vector<float> ring;            // interleaved float32 stereo
    std::size_t writePos = 0, readPos = 0, available = 0;

    std::atomic<float> peak{0.0f};
    AudioPassthrough* owner = nullptr;

    Impl() {
        queue = dispatch_queue_create("dev.ps5directview.audio",
                                      DISPATCH_QUEUE_SERIAL);
        ring.resize(kRingFrames * 2, 0.0f);
    }
    ~Impl() { StopOutput(); StopInput(); }

    // ---- Ring buffer (SPSC over mutex; CoreAudio IO thread is the consumer).
    void WriteSamples(const float* src, std::size_t frames) {
        std::lock_guard<std::mutex> lock(ringMutex);
        for (std::size_t i = 0; i < frames * 2; ++i) {
            ring[writePos] = src[i];
            writePos = (writePos + 1) % ring.size();
            if (available < ring.size()) ++available;
            else readPos = (readPos + 1) % ring.size();
        }
    }
    std::size_t ReadSamples(float* dst, std::size_t frames) {
        std::lock_guard<std::mutex> lock(ringMutex);
        std::size_t want = frames * 2;
        std::size_t got = std::min(want, available);
        for (std::size_t i = 0; i < got; ++i) {
            dst[i] = ring[readPos];
            readPos = (readPos + 1) % ring.size();
        }
        for (std::size_t i = got; i < want; ++i) dst[i] = 0.0f;
        available -= got;
        return got / 2;
    }

    // ---- CoreAudio output unit.
    static OSStatus RenderCallback(void* refcon, AudioUnitRenderActionFlags*,
                                   const AudioTimeStamp*, UInt32,
                                   UInt32 inNumberFrames, AudioBufferList* ioData) {
        auto* self = static_cast<Impl*>(refcon);
        float* dst = static_cast<float*>(ioData->mBuffers[0].mData);
        self->ReadSamples(dst, inNumberFrames);
        float v = self->owner ? self->owner->volume_.load() : 1.0f;
        if (self->owner && self->owner->muted_.load()) v = 0.0f;
        float peak = 0.0f;
        const std::size_t total = inNumberFrames * 2;
        for (std::size_t i = 0; i < total; ++i) {
            dst[i] *= v;
            peak = std::max(peak, std::fabs(dst[i]));
        }
        self->peak.store(peak);
        return noErr;
    }

    bool StartOutput() {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) return false;
        if (AudioComponentInstanceNew(comp, &outputUnit) != noErr) return false;

        outputFormat.mSampleRate = 48000.0;
        outputFormat.mFormatID = kAudioFormatLinearPCM;
        outputFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        outputFormat.mChannelsPerFrame = 2;
        outputFormat.mBitsPerChannel = 32;
        outputFormat.mBytesPerFrame = outputFormat.mChannelsPerFrame * 4;
        outputFormat.mFramesPerPacket = 1;
        outputFormat.mBytesPerPacket = outputFormat.mBytesPerFrame;

        if (AudioUnitSetProperty(outputUnit, kAudioUnitProperty_StreamFormat,
                                 kAudioUnitScope_Input, 0, &outputFormat,
                                 sizeof(outputFormat)) != noErr) return false;

        AURenderCallbackStruct cb{};
        cb.inputProc = RenderCallback;
        cb.inputProcRefCon = this;
        if (AudioUnitSetProperty(outputUnit, kAudioUnitProperty_SetRenderCallback,
                                 kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr)
            return false;

        if (AudioUnitInitialize(outputUnit) != noErr) return false;
        if (AudioOutputUnitStart(outputUnit) != noErr) return false;
        return true;
    }

    void StopOutput() {
        if (!outputUnit) return;
        AudioOutputUnitStop(outputUnit);
        AudioUnitUninitialize(outputUnit);
        AudioComponentInstanceDispose(outputUnit);
        outputUnit = nullptr;
    }

    bool StartInput(const std::string& deviceUniqueID) {
        AVCaptureDevice* dev = [AVCaptureDevice
            deviceWithUniqueID:[NSString stringWithUTF8String:deviceUniqueID.c_str()]];
        // If video device has no audio, search for sibling by name.
        if (!dev || ![dev hasMediaType:AVMediaTypeAudio]) {
            for (AVCaptureDevice* d in [AVCaptureDevice devicesWithMediaType:AVMediaTypeAudio]) {
                dev = d;
                break;
            }
        }
        if (!dev) return false;

        NSError* err = nil;
        AVCaptureDeviceInput* in = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
        if (!in) return false;
        AVCaptureSession* sess = [[AVCaptureSession alloc] init];
        [sess addInput:in];

        AVCaptureAudioDataOutput* out = [[AVCaptureAudioDataOutput alloc] init];
        delegate = [DirectViewAudioDelegate new];
        delegate.impl = this;
        [out setSampleBufferDelegate:delegate queue:queue];
        [sess addOutput:out];
        [sess startRunning];
        session = sess;
        return true;
    }
    void StopInput() {
        if (!session) return;
        [session stopRunning];
        for (AVCaptureInput* in in session.inputs) [session removeInput:in];
        for (AVCaptureOutput* out in session.outputs) [session removeOutput:out];
        session = nil;
        if (delegate) { delegate.impl = nullptr; delegate = nil; }
    }

    void HandleSample(CMSampleBufferRef sample) {
        CMAudioFormatDescriptionRef desc =
            (CMAudioFormatDescriptionRef)CMSampleBufferGetFormatDescription(sample);
        if (!desc) return;
        const AudioStreamBasicDescription* asbd =
            CMAudioFormatDescriptionGetStreamBasicDescription(desc);
        if (!asbd) return;

        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sample);
        if (!bb) return;
        size_t bytes = 0;
        char* src = nullptr;
        if (CMBlockBufferGetDataPointer(bb, 0, nullptr, &bytes, &src) != kCMBlockBufferNoErr)
            return;

        const UInt32 chans = asbd->mChannelsPerFrame;
        const bool isFloat = (asbd->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
        const UInt32 bps = asbd->mBitsPerChannel;
        const std::size_t frameBytes = asbd->mBytesPerFrame;
        const std::size_t frames = bytes / std::max<std::size_t>(frameBytes, 1);

        std::vector<float> out(frames * 2);
        if (isFloat && bps == 32) {
            const float* in = reinterpret_cast<const float*>(src);
            for (std::size_t i = 0; i < frames; ++i) {
                float l = in[i * chans];
                float r = (chans > 1) ? in[i * chans + 1] : l;
                out[i * 2] = l;
                out[i * 2 + 1] = r;
            }
        } else if (bps == 16) {
            const std::int16_t* in = reinterpret_cast<const std::int16_t*>(src);
            constexpr float kScale = 1.0f / 32768.0f;
            for (std::size_t i = 0; i < frames; ++i) {
                float l = in[i * chans] * kScale;
                float r = (chans > 1) ? in[i * chans + 1] * kScale : l;
                out[i * 2] = l;
                out[i * 2 + 1] = r;
            }
        } else {
            return; // unsupported format
        }
        WriteSamples(out.data(), frames);
    }
};

@implementation DirectViewAudioDelegate
- (void)captureOutput:(AVCaptureOutput*)output
  didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
         fromConnection:(AVCaptureConnection*)connection {
    (void)output; (void)connection;
    if (self.impl) self.impl->HandleSample(sampleBuffer);
}
@end

std::vector<AudioOutput> ListAudioOutputs() {
    std::vector<AudioOutput> out;
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
        return out;
    const UInt32 count = size / sizeof(AudioDeviceID);
    std::vector<AudioDeviceID> ids(count);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
        return out;
    for (AudioDeviceID id : ids) {
        // Only devices with output streams.
        AudioObjectPropertyAddress streams = {
            kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput,
            kAudioObjectPropertyElementMain };
        UInt32 streamSize = 0;
        if (AudioObjectGetPropertyDataSize(id, &streams, 0, nullptr, &streamSize) != noErr) continue;
        if (streamSize == 0) continue;

        AudioObjectPropertyAddress nameAddr = {
            kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        CFStringRef nameRef = nullptr;
        UInt32 nameSize = sizeof(nameRef);
        if (AudioObjectGetPropertyData(id, &nameAddr, 0, nullptr, &nameSize, &nameRef) != noErr)
            continue;
        AudioObjectPropertyAddress uidAddr = {
            kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        CFStringRef uidRef = nullptr;
        UInt32 uidSize = sizeof(uidRef);
        AudioObjectGetPropertyData(id, &uidAddr, 0, nullptr, &uidSize, &uidRef);

        AudioOutput entry;
        if (nameRef) {
            entry.name = [(__bridge NSString*)nameRef UTF8String] ?: "";
            CFRelease(nameRef);
        }
        if (uidRef) {
            entry.id = [(__bridge NSString*)uidRef UTF8String] ?: "";
            CFRelease(uidRef);
        }
        out.push_back(std::move(entry));
    }
    return out;
}

AudioPassthrough::AudioPassthrough() = default;
AudioPassthrough::~AudioPassthrough() { Stop(); }

std::string AudioPassthrough::Status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void AudioPassthrough::SetStatus(const std::string& value) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = value;
}

void AudioPassthrough::SetOutput(const std::string& id) {
    std::lock_guard<std::mutex> lock(outputMutex_);
    if (id == outputId_) return;
    outputId_ = id;
    ++outputVersion_;
}

void AudioPassthrough::Start(const std::string& videoUniqueID) {
    Stop();
    impl_ = std::make_unique<Impl>();
    impl_->owner = this;
    running_ = true;
    peak_ = 0.0f;
    SetStatus("Audio starting");
    if (!impl_->StartOutput()) { SetStatus("Audio output init failed"); running_ = false; return; }
    if (!impl_->StartInput(videoUniqueID)) { SetStatus("Audio input init failed"); running_ = false; return; }
    SetStatus("Audio running");
}

void AudioPassthrough::Stop() {
    if (impl_) {
        impl_->StopInput();
        impl_->StopOutput();
        impl_.reset();
    }
    running_ = false;
    peak_ = 0.0f;
    SetStatus("Audio off");
}
