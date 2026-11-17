#pragma once

#include <windows.h>
#include <mfobjects.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct VideoDevice {
    std::wstring name;
    std::wstring symbolicLink;
};

// Requires COM and MFStartup on the calling thread before enumeration.
std::vector<VideoDevice> EnumerateVideoDevices();

// Pixel layouts the renderer can consume directly. YUY2/NV12 are converted to
// RGB on the GPU, so Media Foundation never runs a CPU color converter.
enum class PixelFormat { BGRA, YUY2, NV12 };

struct FrameFormat {
    PixelFormat pixel = PixelFormat::BGRA;
    unsigned width = 0;
    unsigned height = 0;    // visible rows (after display-aperture crop)
    unsigned codedHeight = 0; // allocated rows; NV12 chroma starts after these
    long stride = 0;        // Bytes per row of plane 0; negative = bottom-up (BGRA only).
    double fps = 0.0;
    bool bt709 = true;      // YUV matrix: BT.709 (HD) or BT.601 (SD / JPEG).
    bool fullRange = false; // YUV range: 0-255 (JPEG) or 16-235 (video).
};

struct VideoFrame {
    // Keeps a GPU decoder surface out of the decoder's pool until uploaded.
    Microsoft::WRL::ComPtr<IMFSample> sample;
    // Either a system-memory buffer or an IMFDXGIBuffer (GPU texture).
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    FrameFormat format;
    LONGLONG sampleTime = 0;  // driver capture time, QPC 100 ns; 0 = unknown
    LONGLONG receiveTime = 0; // when the app received it, same clock
    std::uint64_t sequence = 0;
};

// One mailbox slot, not a queue: producer overwrites old images, so the
// display always shows the newest frame and never builds a backlog.
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

class CaptureService {
public:
    explicit CaptureService(HANDLE frameEvent);
    ~CaptureService();
    CaptureService(const CaptureService&) = delete;
    CaptureService& operator=(const CaptureService&) = delete;

    // With a device manager, MJPEG is decoded by the GPU's hardware decoder
    // and frames stay in video memory. Without one, the CPU path is used.
    void SetDeviceManager(IMFDXGIDeviceManager* manager) { deviceManager_ = manager; }
    void Start(std::wstring deviceSymbolicLink);
    void Stop();
    bool TryConsume(VideoFrame& out) { return latest_.Consume(out); }
    std::wstring Status() const;
    // Short format summary for the UI, e.g. "1920x1080 · 60 fps · NV12 · GPU".
    std::wstring Signal() const;
    std::uint64_t FramesReceived() const { return framesReceived_.load(); }
    bool Running() const { return running_.load(); }

private:
    void Worker(std::wstring deviceSymbolicLink);
    void SetStatus(const std::wstring& value);
    void SetSignal(const std::wstring& value);
    std::wstring signal_;

    HANDLE frameEvent_ = nullptr; // Owned by the app.
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    LatestFrame latest_;
    mutable std::mutex statusMutex_;
    std::wstring status_ = L"Select a capture device from the Capture menu.";
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> framesReceived_{0};
};
