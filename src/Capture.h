#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
  #include <mfobjects.h>
  #include <mfapi.h>
  #include <mfidl.h>
  #include <wrl/client.h>
#endif

struct VideoDevice {
#ifdef _WIN32
    std::wstring name;
    std::wstring symbolicLink;
#else
    std::string name;         // AVCaptureDevice localizedName
    std::string symbolicLink; // AVCaptureDevice uniqueID
#endif
};

std::vector<VideoDevice> EnumerateVideoDevices();

enum class PixelFormat { BGRA, YUY2, NV12 };

struct FrameFormat {
    PixelFormat pixel = PixelFormat::BGRA;
    unsigned width = 0;
    unsigned height = 0;
    unsigned codedHeight = 0;
    long stride = 0;
    double fps = 0.0;
    bool bt709 = true;
    bool fullRange = false;
};

#ifdef _WIN32

struct VideoFrame {
    Microsoft::WRL::ComPtr<IMFSample> sample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    FrameFormat format;
    LONGLONG sampleTime = 0;
    LONGLONG receiveTime = 0;
    std::uint64_t sequence = 0;
};

#else

// macOS: a CVPixelBuffer held via void* (bridge-retained by Capture_Mac.mm).
struct VideoFrame {
    void* pixelBuffer = nullptr;   // CVPixelBufferRef, retained
    FrameFormat format;
    std::uint64_t sampleTime = 0;  // host time, mach_absolute_time ns
    std::uint64_t receiveTime = 0;
    std::uint64_t sequence = 0;

    VideoFrame() = default;
    ~VideoFrame();
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&& other) noexcept;
    VideoFrame& operator=(VideoFrame&& other) noexcept;
    void Reset();
};

#endif

class LatestFrame {
public:
    void Publish(VideoFrame&& frame);
    bool Consume(VideoFrame& destination);
    void Clear();

private:
    std::mutex mutex_;
    VideoFrame latest_;
    std::uint64_t nextSequence_ = 0;
};

#ifdef _WIN32

class CaptureService {
public:
    explicit CaptureService(HANDLE frameEvent);
    ~CaptureService();
    CaptureService(const CaptureService&) = delete;
    CaptureService& operator=(const CaptureService&) = delete;

    void SetDeviceManager(IMFDXGIDeviceManager* manager) { deviceManager_ = manager; }
    void Start(std::wstring deviceSymbolicLink);
    void Stop();
    bool TryConsume(VideoFrame& out) { return latest_.Consume(out); }
    std::wstring Status() const;
    std::wstring Signal() const;
    std::uint64_t FramesReceived() const { return framesReceived_.load(); }
    bool Running() const { return running_.load(); }

private:
    void Worker(std::wstring deviceSymbolicLink);
    void SetStatus(const std::wstring& value);
    void SetSignal(const std::wstring& value);
    std::wstring signal_;

    HANDLE frameEvent_ = nullptr;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    LatestFrame latest_;
    mutable std::mutex statusMutex_;
    std::wstring status_ = L"Select a capture device from the Capture menu.";
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> framesReceived_{0};
};

#else

// macOS CaptureService: AVFoundation-backed. The sample-buffer delegate calls
// FrameEvent::Signal() so the main thread can wake and consume the latest
// frame; the window run loop pumps it via a 1 ms NSTimer in Ui_Mac.
class CaptureService {
public:
    struct FrameEvent {
        std::mutex m;
        std::condition_variable cv;
        bool pending = false;
        void Signal() {
            {
                std::lock_guard<std::mutex> lock(m);
                pending = true;
            }
            cv.notify_all();
        }
        bool Wait(std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(m);
            bool got = cv.wait_for(lock, timeout, [&] { return pending; });
            pending = false;
            return got;
        }
    };
    explicit CaptureService(FrameEvent* frameEvent);
    ~CaptureService();
    CaptureService(const CaptureService&) = delete;
    CaptureService& operator=(const CaptureService&) = delete;

    void Start(std::string deviceUniqueID);
    void Stop();
    bool TryConsume(VideoFrame& out) { return latest_.Consume(out); }
    std::string Status() const;
    std::string Signal() const;
    std::uint64_t FramesReceived() const { return framesReceived_.load(); }
    bool Running() const { return running_.load(); }

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    FrameEvent* frameEvent_ = nullptr;
    LatestFrame latest_;
    mutable std::mutex statusMutex_;
    std::string status_ = "Select a capture device from the Capture menu.";
    std::string signal_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> framesReceived_{0};
    friend struct Impl;
};

#endif
