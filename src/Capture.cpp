#include "Capture.h"

#ifdef _WIN32
#include <avrt.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
std::wstring HResultText(HRESULT hr) {
    std::wostringstream ss;
    ss << L"0x" << std::hex << std::uppercase
       << std::setw(8) << std::setfill(L'0')
       << static_cast<unsigned long>(hr);
    return ss.str();
}

void Check(HRESULT hr, const wchar_t* where) {
    if (FAILED(hr)) {
        std::wstring message = std::wstring(where) + L" failed: " + HResultText(hr);
        throw std::runtime_error(std::string(message.begin(), message.end()));
    }
}

std::wstring GetString(IMFAttributes* attrs, const GUID& key) {
    wchar_t* value = nullptr;
    UINT32 length = 0;
    const HRESULT hr = attrs->GetAllocatedString(key, &value, &length);
    if (FAILED(hr)) return {};
    std::wstring result(value, length);
    CoTaskMemFree(value);
    return result;
}

struct NativeMode {
    ComPtr<IMFMediaType> type;
    unsigned width = 0;
    unsigned height = 0;
    double fps = 0.0;
    GUID subtype{};
    long long score = 0;
};

bool IsUncompressed(const GUID& subtype) {
    return subtype == MFVideoFormat_YUY2 || subtype == MFVideoFormat_NV12 ||
           subtype == MFVideoFormat_RGB32;
}

long long ModeScore(const NativeMode& mode) {
    // Low latency matters more than resolution. Frame-rate tier dominates,
    // then 1080p (what PS5 capture cards deliver), then uncompressed formats,
    // which skip the MJPEG decode step entirely.
    const long long pixels = static_cast<long long>(mode.width) * mode.height;
    long long score = 0;
    // 120 Hz only wins at full 1080p: 720p120 would trade sharpness for a
    // few ms, so 1080p60 beats it.
    if (mode.fps >= 119.0 && mode.height >= 1080) score += 40'000'000;
    else if (mode.fps >= 59.0) score += 30'000'000;
    else if (mode.fps >= 29.0) score += 20'000'000;
    else if (mode.fps >= 24.0) score += 10'000'000;
    else if (mode.fps >= 15.0) score += 5'000'000;
    if (mode.width == 1920 && mode.height == 1080) score += 1'000'000;
    else if (mode.width == 1280 && mode.height == 720) score += 700'000;
    else if (pixels > 0 && pixels <= 1920LL * 1080)
        score += 200'000 + pixels / 10;
    else if (pixels > 0) score += 100'000; // avoid 4K input on cheap adapters
    // Tie-break close modes by nominal FPS, then prefer uncompressed formats.
    score += static_cast<long long>(std::clamp(mode.fps, 0.0, 240.0) * 100.0);
    if (IsUncompressed(mode.subtype)) score += 500;    if (mode.subtype == MFVideoFormat_NV12 || mode.subtype == MFVideoFormat_YUY2)
        score += 100; // GPU converts these; RGB32 costs twice the bandwidth.
    return score;
}

std::vector<NativeMode> GetModes(IMFSourceReader* reader) {
    std::vector<NativeMode> result;
    for (DWORD i = 0; i < 256; ++i) {
        ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                       i, &type);
        if (hr == MF_E_NO_MORE_TYPES) break;
        if (FAILED(hr)) continue;
        NativeMode mode;
        mode.type = type;
        UINT32 w = 0, h = 0, n = 0, d = 0;
        if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h))) continue;
        mode.width = w;
        mode.height = h;
        if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &n, &d)) && d)
            mode.fps = static_cast<double>(n) / d;
        type->GetGUID(MF_MT_SUBTYPE, &mode.subtype);
        mode.score = ModeScore(mode);
        result.push_back(std::move(mode));
    }
    std::stable_sort(result.begin(), result.end(),
        [](const NativeMode& a, const NativeMode& b) { return a.score > b.score; });
    return result;
}

const GUID& SubtypeFor(PixelFormat pixel) {
    switch (pixel) {
        case PixelFormat::YUY2: return MFVideoFormat_YUY2;
        case PixelFormat::NV12: return MFVideoFormat_NV12;
        default: return MFVideoFormat_RGB32;
    }
}

unsigned Plane0BytesPerPixel(PixelFormat pixel) {
    switch (pixel) {
        case PixelFormat::YUY2: return 2;
        case PixelFormat::NV12: return 1;
        default: return 4;
    }
}

// Reads the reader's current output type. A decoder is allowed to change its
// output media type while streaming (e.g. first frame of an H.264 MP4); a
// format-change flag is an event to handle, not a reason to stop capture.
bool ReadOutputFormat(IMFSourceReader* reader, double fallbackFps,
                      const GUID& nativeSubtype, FrameFormat& result) {
    ComPtr<IMFMediaType> output;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                           &output))) return false;
    GUID subtype{};
    if (FAILED(output->GetGUID(MF_MT_SUBTYPE, &subtype))) return false;

    FrameFormat format;
    if (subtype == MFVideoFormat_YUY2) format.pixel = PixelFormat::YUY2;
    else if (subtype == MFVideoFormat_NV12) format.pixel = PixelFormat::NV12;
    else if (subtype == MFVideoFormat_RGB32) format.pixel = PixelFormat::BGRA;
    else return false;

    UINT32 width = 0, height = 0;
    if (FAILED(MFGetAttributeSize(output.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
        width == 0 || height == 0 || width > 8192 || height > 8192) return false;
    const UINT32 codedHeight = height;
    // Decoders pad surfaces (1920x1088 for 1080p). Crop to the display
    // aperture so no garbage rows reach the screen; plane layout is unchanged
    // because the renderer simply copies fewer rows from the top.
    MFVideoArea aperture{};
    if (SUCCEEDED(output->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                  reinterpret_cast<UINT8*>(&aperture),
                                  sizeof(aperture), nullptr)) &&
        aperture.OffsetX.value == 0 && aperture.OffsetY.value == 0 &&
        aperture.Area.cx > 0 && aperture.Area.cy > 0 &&
        static_cast<UINT32>(aperture.Area.cx) <= width &&
        static_cast<UINT32>(aperture.Area.cy) <= height) {
        width = static_cast<UINT32>(aperture.Area.cx);
        height = static_cast<UINT32>(aperture.Area.cy);
    }
    if (format.pixel != PixelFormat::BGRA && ((width & 1) || (height & 1))) return false;

    format.width = width;
    format.height = height;
    format.codedHeight = codedHeight;
    format.fps = fallbackFps;
    UINT32 numerator = 0, denominator = 0;
    if (SUCCEEDED(MFGetAttributeRatio(output.Get(), MF_MT_FRAME_RATE,
                                     &numerator, &denominator)) && denominator)
        format.fps = static_cast<double>(numerator) / denominator;

    const long minPitch = static_cast<long>(width * Plane0BytesPerPixel(format.pixel));
    UINT32 rawStride = 0;
    if (SUCCEEDED(output->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride))) {
        format.stride = static_cast<long>(static_cast<std::int32_t>(rawStride));
    } else {
        LONG computed = 0;
        if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(subtype.Data1, width, &computed)))
            format.stride = computed;
    }
    if (format.stride == 0) format.stride = minPitch;
    if (format.pixel != PixelFormat::BGRA && format.stride < 0) return false;
    if (std::abs(format.stride) < minPitch) return false;

    if (nativeSubtype == MFVideoFormat_MJPG) {
        // JPEG (JFIF) is always BT.601 full-range YCbCr, and decoders hand the
        // planes through untouched. Cheap cards (MS2109 etc.) report "pc"
        // range; their decoder output tags are unreliable, so ignore them.
        format.bt709 = false;
        format.fullRange = true;
    } else {
        // Uncompressed capture is video-range; HD defaults to BT.709.
        format.bt709 = height >= 720;
        UINT32 matrix = 0;
        if (SUCCEEDED(output->GetUINT32(MF_MT_YUV_MATRIX, &matrix))) {
            if (matrix == MFVideoTransferMatrix_BT601) format.bt709 = false;
            else if (matrix == MFVideoTransferMatrix_BT709) format.bt709 = true;
        }
        UINT32 range = 0;
        if (SUCCEEDED(output->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range)))
            format.fullRange = range == MFNominalRange_0_255;
    }

    result = format;
    return true;
}

bool RequestOutput(IMFSourceReader* reader, const GUID& subtype,
                   const NativeMode* native) {
    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateMediaType(&type))) return false;
    if (FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) return false;
    if (FAILED(type->SetGUID(MF_MT_SUBTYPE, subtype))) return false;
    if (native) {
        // Keep dimensions and frame rate explicit. Otherwise a Source Reader
        // can negotiate the output at an unintended default resolution.
        if (FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                                      native->width, native->height))) return false;
        UINT32 n = 0, d = 0;
        if (SUCCEEDED(MFGetAttributeRatio(native->type.Get(), MF_MT_FRAME_RATE, &n, &d)) && d)
            MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, n, d);
    }
    return SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                 nullptr, type.Get()));
}

// Output formats to try, cheapest first. RGB32 is the last-resort fallback.
// Uncompressed sources keep their own layout (no transform at all). For MJPEG,
// GPU decoders natively write NV12; the CPU decoder natively writes YUY2.
std::vector<GUID> OutputCandidates(const GUID& nativeSubtype, bool gpu) {
    std::vector<GUID> list;
    if (nativeSubtype == MFVideoFormat_YUY2) {
        list = {MFVideoFormat_YUY2, MFVideoFormat_NV12};
    } else if (nativeSubtype == MFVideoFormat_NV12 || gpu) {
        list = {MFVideoFormat_NV12, MFVideoFormat_YUY2};
    } else {
        list = {MFVideoFormat_YUY2, MFVideoFormat_NV12};
    }
    list.push_back(MFVideoFormat_RGB32);
    return list;
}

// Media Foundation may momentarily reset the decoder's output type. Restore a
// format the renderer understands, preferring the one already in use.
void RestoreOutput(IMFSourceReader* reader, PixelFormat preferred) {
    if (RequestOutput(reader, SubtypeFor(preferred), nullptr)) return;
    for (const GUID& subtype : OutputCandidates(GUID_NULL, false))
        if (RequestOutput(reader, subtype, nullptr)) return;
    Check(E_FAIL, L"Restore video output type");
}

FrameFormat Negotiate(IMFSourceReader* reader, bool gpu, GUID& nativeSubtype) {
    const auto modes = GetModes(reader);
    if (modes.empty()) throw std::runtime_error("Capture device provides no video modes.");

    ComPtr<IMFSourceReaderEx> readerEx;
    reader->QueryInterface(IID_PPV_ARGS(&readerEx));
    for (const auto& native : modes) {
        // Program the device itself into this mode (resolution, fps, codec),
        // then negotiate the decoded output separately.
        DWORD changes = 0;
        if (!readerEx || FAILED(readerEx->SetNativeMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, native.type.Get(), &changes))) {
            if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                   nullptr, native.type.Get()))) continue;
        }
        for (const GUID& subtype : OutputCandidates(native.subtype, gpu)) {
            if (!RequestOutput(reader, subtype, &native)) continue;
            FrameFormat format;
            if (ReadOutputFormat(reader, native.fps, native.subtype, format)) {
                nativeSubtype = native.subtype;
                return format;
            }
        }
    }
    throw std::runtime_error(
        "Cannot negotiate a video format. Device may lack a compatible MJPEG/YUY2 decoder "
        "or video processor. Try another USB 3.0 port or capture device.");
}

std::wstring ShortDescribe(const FrameFormat& format, const GUID& nativeSubtype,
                          const wchar_t* decodePath) {
    std::wostringstream s;
    s << format.width << L"\x00D7" << format.height << L"  \x00B7  "
      << static_cast<int>(format.fps + 0.5) << L" fps  \x00B7  "
      << (nativeSubtype == MFVideoFormat_MJPG ? L"MJPEG" :
          nativeSubtype == MFVideoFormat_YUY2 ? L"YUY2" :
          nativeSubtype == MFVideoFormat_NV12 ? L"NV12" : L"Video")
      << L"  \x00B7  " << decodePath;
    return s.str();
}

// decodePath: e.g. "GPU decode", "CPU decode", "no decode".
std::wstring Describe(const FrameFormat& format, const GUID& nativeSubtype,
                      const wchar_t* decodePath, bool fileMode) {
    std::wostringstream detail;
    detail << L"Capturing " << format.width << L"x" << format.height
           << L" (source " << std::fixed << std::setprecision(1)
           << format.fps << L" fps "
           << (nativeSubtype == MFVideoFormat_MJPG ? L"MJPEG" :
               nativeSubtype == MFVideoFormat_YUY2 ? L"YUY2" :
               nativeSubtype == MFVideoFormat_NV12 ? L"NV12" : L"other")
           << L" -> "
           << (format.pixel == PixelFormat::YUY2 ? L"YUY2" :
               format.pixel == PixelFormat::NV12 ? L"NV12" : L"RGB32")
           << L", " << decodePath << L")";
    if (fileMode) detail << L" | TEST FILE (not HDMI latency)";
    return detail.str();
}
} // namespace

std::vector<VideoDevice> EnumerateVideoDevices() {
    ComPtr<IMFAttributes> attrs;
    Check(MFCreateAttributes(&attrs, 1), L"MFCreateAttributes");
    Check(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
          L"Set video device source type");

    IMFActivate** rawActivations = nullptr;
    UINT32 count = 0;
    Check(MFEnumDeviceSources(attrs.Get(), &rawActivations, &count), L"MFEnumDeviceSources");
    std::vector<VideoDevice> result;
    for (UINT32 i = 0; i < count; ++i) {
        auto name = GetString(rawActivations[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        auto link = GetString(rawActivations[i],
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        if (!link.empty()) result.push_back({std::move(name), std::move(link)});
        rawActivations[i]->Release();
    }
    CoTaskMemFree(rawActivations);
    return result;
}


void LatestFrame::Publish(VideoFrame&& frame) {
    if (!frame.buffer || !frame.format.width || !frame.format.height) return;
    std::lock_guard<std::mutex> lock(mutex_);
    // Overwrite: an unconsumed older frame is dropped, never queued.
    const auto sequence = ++nextSequence_;
    latest_ = std::move(frame);
    latest_.sequence = sequence;
}

bool LatestFrame::Consume(VideoFrame& destination) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_.buffer || latest_.sequence == destination.sequence)
        return false;
    destination = std::move(latest_);
    latest_.sample.Reset();
    latest_.buffer.Reset();
    latest_.sequence = destination.sequence;
    return true;
}

void LatestFrame::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_.sample.Reset();
    latest_.buffer.Reset();
    latest_.format = {};
    latest_.sequence = ++nextSequence_;
}

CaptureService::CaptureService(HANDLE frameEvent) : frameEvent_(frameEvent) {}
CaptureService::~CaptureService() { Stop(); }

void CaptureService::SetStatus(const std::wstring& value) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = value;
    if (frameEvent_) SetEvent(frameEvent_);
}

void CaptureService::SetSignal(const std::wstring& value) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    signal_ = value;
}

std::wstring CaptureService::Signal() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return signal_;
}

std::wstring CaptureService::Status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void CaptureService::Start(std::wstring link) {
    Stop();
    latest_.Clear();
    framesReceived_ = 0;
    stop_ = false;
    running_ = true;
    SetStatus(L"Opening capture device...");
    worker_ = std::thread(&CaptureService::Worker, this, std::move(link));
}

void CaptureService::Stop() {
    // The worker sees stop_ on its next frame (<= 1 frame time). Do NOT shut
    // the media source down from here: a synchronous ReadSample then never
    // returns (measured: shutdown hang). The worker tears it down itself.
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    running_ = false;
}

void CaptureService::Worker(std::wstring link) {
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initHr)) {
        SetStatus(L"COM initialization failed: " + HResultText(initHr));
        running_ = false;
        return;
    }
    // Register with MMCSS so the scheduler services this thread promptly
    // when the driver delivers a frame, even while the GPU/CPU is busy.
    DWORD mmcssTask = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &mmcssTask);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
    else SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    try {
        // All COM objects are destroyed before CoUninitialize.
        [&] {
            // A local test video shares the normal Media Foundation / D3D11 path.
            // Only the input source and timestamp pacing differ from a live USB device.
            const bool fileMode = link.starts_with(L"FILE:");

            ComPtr<IMFSourceReader> reader;
            ComPtr<IMFMediaSource> source;
            GUID nativeSubtype = GUID_NULL;
            FrameFormat format;
            auto open = [&](bool gpu) {
                reader.Reset();
                if (source) { source->Shutdown(); source.Reset(); }
                if (stop_) throw std::runtime_error("Capture stopped.");
                ComPtr<IMFAttributes> readerAttrs;
                Check(MFCreateAttributes(&readerAttrs, 4), L"MFCreateAttributes(reader)");
                // Lets the reader insert a decoder/converter only when needed.
                Check(readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,
                                             TRUE), L"Enable video processing");
                // Tells decoders and the pipeline not to buffer frames ahead.
                readerAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
                if (gpu) {
                    // Hardware MJPEG decoder (NVIDIA/AMD/Intel MFT) writes
                    // straight into GPU textures on the renderer's device.
                    Check(readerAttrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER,
                                                  deviceManager_.Get()), L"Set D3D manager");
                    readerAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
                }
                if (fileMode) {
                    const std::wstring path = link.substr(5);
                    Check(MFCreateSourceReaderFromURL(path.c_str(), readerAttrs.Get(),
                                                      &reader), L"Open test video");
                } else {
                    ComPtr<IMFAttributes> sourceAttrs;
                    Check(MFCreateAttributes(&sourceAttrs, 2), L"MFCreateAttributes(source)");
                    Check(sourceAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID), L"Source type");
                    Check(sourceAttrs->SetString(
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                            link.c_str()), L"Source symbolic link");
                    Check(MFCreateDeviceSource(sourceAttrs.Get(), &source),
                          L"MFCreateDeviceSource");
                    Check(MFCreateSourceReaderFromMediaSource(source.Get(),
                          readerAttrs.Get(), &reader), L"MFCreateSourceReader");
                }
                reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
                Check(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE),
                      L"Select video stream");
                format = Negotiate(reader.Get(), gpu, nativeSubtype);
            };

            bool gpu = deviceManager_ != nullptr;
            if (gpu) {
                try { open(true); }
                catch (const std::exception&) { gpu = false; }
            }
            if (!gpu) open(false);

            // Decode path is known for sure once the first sample arrives:
            // GPU-decoded frames come back as IMFDXGIBuffer textures.
            const wchar_t* decodePath = L"starting";
            bool pathKnown = false;
            SetStatus(Describe(format, nativeSubtype, decodePath, fileMode));
                            SetSignal(ShortDescribe(format, nativeSubtype, decodePath));

            // Decoded file samples can arrive much faster than real-time. Pace
            // them using file timestamps so the FPS counter represents actual
            // 60-Hz playback rather than how fast the CPU can decode a clip.
            bool firstTimedFrame = true;
            LONGLONG firstSampleTime = 0;
            std::chrono::steady_clock::time_point fileWallStart{};
            std::uint64_t gpuFailures = 0;
            while (!stop_.load(std::memory_order_relaxed)) {
                DWORD streamFlags = 0;
                LONGLONG timestamp = 0;
                ComPtr<IMFSample> sample;
                const HRESULT hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                    0, nullptr, &streamFlags, &timestamp, &sample);
                if (FAILED(hr)) {
                    if (gpu && framesReceived_ == 0 && ++gpuFailures < 2) {
                        // Hardware decoder refused this stream: retry on CPU.
                        gpu = false;
                        open(false);
                        pathKnown = false;
                        continue;
                    }
                    SetStatus(L"Capture read failed: " + HResultText(hr));
                    break;
                }
                if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
                    if (fileMode) {
                        PROPVARIANT position{};
                        position.vt = VT_I8;
                        position.hVal.QuadPart = 0;
                        const HRESULT seekHr = reader->SetCurrentPosition(GUID_NULL,
                                                                           position);
                        if (SUCCEEDED(seekHr)) {
                            firstTimedFrame = true;
                            continue; // Loop local test files until device switched.
                        }
                    }
                    SetStatus(fileMode ? L"Test video ended (could not loop)." :
                                         L"Capture device ended the video stream.");
                    break;
                }
                if (streamFlags & MF_SOURCE_READERF_ERROR) {
                    SetStatus(L"Media Foundation reported an unrecoverable read error.");
                    break;
                }
                if (streamFlags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                                   MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) {
                    // Normal for compressed files when the decoder discovers
                    // the actual dimensions of the first frame, and for capture
                    // cards when the PS5 switches resolution / refresh rate.
                    FrameFormat changed;
                    bool outputReset = false;
                    if (!ReadOutputFormat(reader.Get(), format.fps, nativeSubtype, changed)) {
                        RestoreOutput(reader.Get(), format.pixel);
                        outputReset = true;
                        Check(ReadOutputFormat(reader.Get(), format.fps, nativeSubtype, changed)
                                  ? S_OK : E_FAIL,
                              L"Read renegotiated video format");
                    }
                    format = changed;
                    SetStatus(Describe(format, nativeSubtype, decodePath, fileMode));
                            SetSignal(ShortDescribe(format, nativeSubtype, decodePath));
                    // If we changed the reader's output type explicitly,
                    // samples returned during the transition may be in the
                    // old format. Drop this one and read the next sample.
                    if (outputReset) continue;
                }
                if (sample && fileMode) {
                    LONGLONG sampleTime = 0;
                    if (SUCCEEDED(sample->GetSampleTime(&sampleTime))) {
                        if (firstTimedFrame) {
                            firstTimedFrame = false;
                            firstSampleTime = sampleTime;
                            fileWallStart = std::chrono::steady_clock::now();
                        }
                        const auto elapsed = std::chrono::duration<double>(
                            static_cast<double>(sampleTime - firstSampleTime) / 10'000'000.0);
                        const auto target = fileWallStart +
                            std::chrono::duration_cast<std::chrono::steady_clock::duration>(elapsed);
                        while (!stop_.load(std::memory_order_relaxed) &&
                               std::chrono::steady_clock::now() < target) {
                            const auto remaining = target - std::chrono::steady_clock::now();
                            std::this_thread::sleep_for(std::min(
                                remaining, std::chrono::steady_clock::duration(
                                    std::chrono::milliseconds(5))));
                        }
                    } else if (format.fps > 0) {
                        std::this_thread::sleep_for(std::chrono::duration<double>(
                            1.0 / format.fps));
                    }
                }
                if (sample && !stop_.load(std::memory_order_relaxed)) {
                    // Single-buffer samples (the norm) return their own buffer
                    // here with no copy. The renderer uploads straight from it.
                    VideoFrame frame;
                    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&frame.buffer))) {
                        if (!pathKnown) {
                            ComPtr<IMFDXGIBuffer> dxgi;
                            const bool onGpu = SUCCEEDED(frame.buffer.As(&dxgi));
                            decodePath = nativeSubtype == MFVideoFormat_MJPG ?
                                (onGpu ? L"GPU decode" : L"CPU decode") :
                                (onGpu ? L"GPU upload" : L"no decode");
                            pathKnown = true;
                            SetStatus(Describe(format, nativeSubtype, decodePath, fileMode));
                            SetSignal(ShortDescribe(format, nativeSubtype, decodePath));
                        }
                        frame.sample = sample;
                        frame.format = format;
                        // Driver capture time (QPC, 100 ns) when the camera
                        // stack provides it; lets the app show true latency.
                        // The ReadSample timestamp is a media clock, not QPC,
                        // so it is never used for latency.
                        UINT64 deviceTime = 0;
                        if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DeviceTimestamp,
                                                        &deviceTime)))
                            frame.sampleTime = static_cast<LONGLONG>(deviceTime);
                        frame.receiveTime = MFGetSystemTime();
                        latest_.Publish(std::move(frame));
                        ++framesReceived_;
                        if (frameEvent_) SetEvent(frameEvent_);
                    }
                }
            }
            // Release any frame still referencing decoder surfaces before the
            // reader (and its sample allocator) is torn down in this thread.
            latest_.Clear();
            reader.Reset();
            if (source) source->Shutdown();
        }();
    } catch (const std::exception& e) {
        const std::string msg(e.what());
        SetStatus(std::wstring(msg.begin(), msg.end()));
    }
    latest_.Clear();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    running_ = false;
    CoUninitialize();
}

#endif // _WIN32
