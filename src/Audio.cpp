#include "Audio.h"

#ifdef _WIN32
#include <initguid.h>
#include <audioclient.h>
#include <avrt.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
// Endpoint property store exposes the same container ID as DEVPKEY.
const PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kPkeyContainerId = {
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

struct AudioError : std::runtime_error {
    HRESULT hr;
    AudioError(const char* where, HRESULT code) : std::runtime_error(where), hr(code) {}
};

void Check(HRESULT hr, const char* where) {
    if (FAILED(hr)) throw AudioError(where, hr);
}

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

// The physical USB device (container) behind a video device interface.
bool VideoContainerId(const std::wstring& link, GUID& container) {
    wchar_t instance[512]{};
    ULONG size = sizeof(instance);
    DEVPROPTYPE type = 0;
    if (CM_Get_Device_Interface_PropertyW(link.c_str(), &DEVPKEY_Device_InstanceId, &type,
            reinterpret_cast<PBYTE>(instance), &size, 0) != CR_SUCCESS) return false;
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return false;
    size = sizeof(container);
    return CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type,
               reinterpret_cast<PBYTE>(&container), &size, 0) == CR_SUCCESS &&
           type == DEVPROP_TYPE_GUID;
}

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return L"?";
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name = L"?";
    if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &value)) &&
        value.vt == VT_LPWSTR) name = value.pwszVal;
    PropVariantClear(&value);
    return name;
}

std::wstring DeviceId(IMMDevice* device) {
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id))) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

// Prefer the endpoint in the same USB container as the video device; fall
// back to a capture-card-looking name ("USB Digital Audio", "HDMI", ...).
ComPtr<IMMDevice> FindCardEndpoint(IMMDeviceEnumerator* enumerator,
                                   const std::wstring& videoLink, bool& byContainer) {
    byContainer = false;
    GUID container{};
    const bool haveContainer = VideoContainerId(videoLink, container);
    ComPtr<IMMDeviceCollection> collection;
    Check(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection),
          "EnumAudioEndpoints");
    UINT count = 0;
    collection->GetCount(&count);
    ComPtr<IMMDevice> byName;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;
        ComPtr<IPropertyStore> props;
        if (haveContainer && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            const bool match = SUCCEEDED(props->GetValue(kPkeyContainerId, &value)) &&
                               value.vt == VT_CLSID && value.puuid &&
                               IsEqualGUID(*value.puuid, container);
            PropVariantClear(&value);
            if (match) { byContainer = true; return device; }
        }
        // Only guess by name when the USB container could not be resolved;
        // otherwise an unrelated HDMI/capture input could be picked up.
        if (haveContainer) continue;
        const std::wstring name = Lower(FriendlyName(device.Get()));
        if (!byName && (name.find(L"usb digital audio") != std::wstring::npos ||
                        name.find(L"hdmi") != std::wstring::npos ||
                        name.find(L"capture") != std::wstring::npos ||
                        name.find(L"cam link") != std::wstring::npos ||
                        name.find(L"elgato") != std::wstring::npos ||
                        name.find(L"avermedia") != std::wstring::npos))
            byName = device;
    }
    return byName;
}

bool IsFloat32(const WAVEFORMATEX* f) {
    if (f->wBitsPerSample != 32) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        return IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

struct CoTaskFormat {
    WAVEFORMATEX* p = nullptr;
    ~CoTaskFormat() { CoTaskMemFree(p); }
};

// Smallest shared-mode period the driver allows (IAudioClient3), else default.
void InitLowLatency(IAudioClient* client, const WAVEFORMATEX* format, DWORD flags,
                    bool allowAc3) {
    ComPtr<IAudioClient3> ac3;
    if (allowAc3 && SUCCEEDED(client->QueryInterface(IID_PPV_ARGS(&ac3)))) {
        UINT32 def = 0, fundamental = 0, minimum = 0, maximum = 0;
        if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(format, &def, &fundamental,
                                                     &minimum, &maximum)) &&
            SUCCEEDED(ac3->InitializeSharedAudioStream(flags, minimum, format, nullptr)))
            return;
    }
    Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, format, nullptr),
          "IAudioClient::Initialize");
}
} // namespace

std::vector<AudioOutput> ListAudioOutputs() {
    std::vector<AudioOutput> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) return result;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)))
        return result;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(collection->Item(i, &device)))
            result.push_back({DeviceId(device.Get()), FriendlyName(device.Get())});
    }
    return result;
}

void AudioPassthrough::SetOutput(const std::wstring& id) {
    std::lock_guard<std::mutex> lock(outputMutex_);
    if (id == outputId_) return;
    outputId_ = id;
    ++outputVersion_;
}

AudioPassthrough::~AudioPassthrough() {
    Stop();
}

void AudioPassthrough::SetStatus(const std::wstring& value) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = value;
}

std::wstring AudioPassthrough::Status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void AudioPassthrough::Start(const std::wstring& videoSymbolicLink) {
    Stop();
    stop_ = false;
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_ = true;
    SetStatus(L"Audio starting");
    worker_ = std::thread(&AudioPassthrough::Worker, this, videoSymbolicLink);
}

void AudioPassthrough::Stop() {
    stop_ = true;
    if (stopEvent_) SetEvent(stopEvent_);
    if (worker_.joinable()) worker_.join();
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    running_ = false;
    SetStatus(L"Audio off");
}

void AudioPassthrough::Worker(std::wstring videoLink) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        SetStatus(L"Audio: COM init failed");
        running_ = false;
        return;
    }
    DWORD mmcssTask = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);

    HANDLE captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Each pass opens card -> default output and streams until the stream
    // breaks (unplug, default output switched, format change), then retries.
    while (!stop_) {
        try {
            ComPtr<IMMDeviceEnumerator> enumerator;
            Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&enumerator)), "MMDeviceEnumerator");
            bool byContainer = false;
            ComPtr<IMMDevice> input = FindCardEndpoint(enumerator.Get(), videoLink, byContainer);
            if (!input) {
                SetStatus(L"Audio: capture card has no audio input");
                WaitForSingleObject(stopEvent_, 2000);
                continue;
            }
            const unsigned version = outputVersion_.load();
            std::wstring wanted;
            {
                std::lock_guard<std::mutex> lock(outputMutex_);
                wanted = outputId_;
            }
            ComPtr<IMMDevice> output;
            const bool followDefault = wanted.empty() ||
                FAILED(enumerator->GetDevice(wanted.c_str(), &output));
            if (followDefault)
                Check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &output),
                      "Default output");
            const std::wstring outputId = DeviceId(output.Get());

            ComPtr<IAudioClient> capture;
            Check(input->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &capture),
                  "Activate capture");
            CoTaskFormat inFormat;
            Check(capture->GetMixFormat(&inFormat.p), "Capture mix format");
            if (!IsFloat32(inFormat.p)) throw AudioError("Capture format", E_FAIL);
            InitLowLatency(capture.Get(), inFormat.p, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, true);
            Check(capture->SetEventHandle(captureEvent), "Capture event");
            ComPtr<IAudioCaptureClient> captureClient;
            Check(capture->GetService(IID_PPV_ARGS(&captureClient)), "Capture service");

            ComPtr<IAudioClient> render;
            Check(output->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &render),
                  "Activate render");
            CoTaskFormat outMix;
            Check(render->GetMixFormat(&outMix.p), "Render mix format");
            // Same sample rate: write float straight into the engine's own
            // format with the lowest period. Otherwise let Windows resample.
            const bool direct = IsFloat32(outMix.p) &&
                                outMix.p->nSamplesPerSec == inFormat.p->nSamplesPerSec;
            const WAVEFORMATEX* outFormat = direct ? outMix.p : inFormat.p;
            DWORD renderFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            if (!direct)
                renderFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                               AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            InitLowLatency(render.Get(), outFormat, renderFlags, direct);
            Check(render->SetEventHandle(renderEvent), "Render event");
            ComPtr<IAudioRenderClient> renderClient;
            Check(render->GetService(IID_PPV_ARGS(&renderClient)), "Render service");
            UINT32 renderBufferFrames = 0;
            Check(render->GetBufferSize(&renderBufferFrames), "Render buffer size");

            const unsigned inCh = inFormat.p->nChannels;
            const unsigned outCh = outFormat->nChannels;
            const double rate = inFormat.p->nSamplesPerSec;

            REFERENCE_TIME capPeriod = 0, renPeriod = 0;
            capture->GetDevicePeriod(&capPeriod, nullptr);
            render->GetDevicePeriod(&renPeriod, nullptr);
            // Jitter cushion between the two clocks: about one capture period.
            // Anything queued beyond 2x is dropped, so latency never creeps
            // up from clock drift between the card and the output device.
            // Adaptive jitter buffer: starts at one capture period, grows
            // 2 ms on every underrun (max 60 ms) and shrinks 1 ms after each
            // 20 s without one, settling at the smallest glitch-free size.
            const size_t minTarget = std::max<size_t>(
                static_cast<size_t>(rate * std::max(capPeriod, renPeriod) / 1e7), 64);
            const size_t maxTarget = static_cast<size_t>(rate * 0.060);
            const size_t step = static_cast<size_t>(rate * 0.002);
            size_t target = minTarget + step * 2;
            auto ceilingFor = [&](size_t t) { return t * 2 + static_cast<size_t>(rate * 0.006); };
            ULONGLONG lastUnderrunTick = GetTickCount64();

            std::vector<float> ring;      // interleaved, input channel layout
            ring.reserve(static_cast<size_t>(rate) * inCh);
            size_t readPos = 0;           // frames consumed from the front
            bool buffering = true;        // refill to target after underrun
            unsigned underruns = 0, skips = 0;

            Check(capture->Start(), "Capture start");
            Check(render->Start(), "Render start");

            const std::wstring route = FriendlyName(input.Get()) + L" -> " +
                                       FriendlyName(output.Get()) +
                                       (byContainer ? L"" : L" [matched by name]");
            ULONGLONG lastCheck = GetTickCount64();
            HANDLE waits[] = {stopEvent_, captureEvent, renderEvent};
            bool restart = false;
            while (!stop_ && !restart) {
                const DWORD wait = WaitForMultipleObjects(3, waits, FALSE, 200);
                if (wait == WAIT_OBJECT_0) break;

                // Output device picked in settings: reopen on the new one.
                if (outputVersion_.load() != version) { restart = true; break; }

                // Drain every captured packet into the ring.
                float packetPeak = 0.0f;
                UINT32 packet = 0;
                while (SUCCEEDED(captureClient->GetNextPacketSize(&packet)) && packet) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    Check(captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr),
                          "Capture GetBuffer");
                    const size_t old = ring.size();
                    ring.resize(old + static_cast<size_t>(frames) * inCh);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                        std::fill(ring.begin() + old, ring.end(), 0.0f);
                    else {
                        std::memcpy(ring.data() + old, data,
                                    static_cast<size_t>(frames) * inCh * sizeof(float));
                        for (size_t k = old; k < ring.size(); ++k)
                            packetPeak = std::max(packetPeak, std::abs(ring[k]));
                    }
                    Check(captureClient->ReleaseBuffer(frames), "Capture ReleaseBuffer");
                }

                peak_ = std::max(packetPeak, peak_.load() * 0.92f);

                UINT32 padding = 0;
                Check(render->GetCurrentPadding(&padding), "Render padding");
                size_t queued = ring.size() / inCh - readPos;
                if (queued + padding > ceilingFor(target)) {
                    // Too far behind real time: skip ahead to the target.
                    const size_t excess = queued + padding - target;
                    readPos += std::min(excess, queued);
                    ++skips;
                    queued = ring.size() / inCh - readPos;
                }
                if (buffering && queued >= target) buffering = false;

                const UINT32 space = renderBufferFrames - padding;
                const UINT32 n = buffering ? 0 :
                    static_cast<UINT32>(std::min<size_t>(space, queued));
                if (n) {
                    BYTE* dst = nullptr;
                    Check(renderClient->GetBuffer(n, &dst), "Render GetBuffer");
                    auto* out = reinterpret_cast<float*>(dst);
                    const float* in = ring.data() + readPos * inCh;
                    // Perceptual volume curve; muted writes silence.
                    const float v = volume_.load();
                    const float gain = muted_.load() ? 0.0f : v * v;
                    if (inCh == outCh && gain == 1.0f) {
                        std::memcpy(out, in, static_cast<size_t>(n) * inCh * sizeof(float));
                    } else {
                        // Stereo into 5.1/7.1 keeps front L/R; mono fills all.
                        for (UINT32 f = 0; f < n; ++f)
                            for (unsigned c = 0; c < outCh; ++c)
                                out[f * outCh + c] = gain * (inCh == 1 ? in[f] :
                                    c < inCh ? in[f * inCh + c] : 0.0f);
                    }
                    Check(renderClient->ReleaseBuffer(n, 0), "Render ReleaseBuffer");
                    readPos += n;
                } else if (!buffering && queued == 0 && padding == 0) {
                    buffering = true; // underrun: rebuild a bigger cushion
                    ++underruns;
                    target = std::min(target + step, maxTarget);
                    lastUnderrunTick = GetTickCount64();
                }
                // Compact the ring occasionally instead of every packet.
                if (readPos > static_cast<size_t>(rate / 4)) {
                    ring.erase(ring.begin(), ring.begin() + readPos * inCh);
                    readPos = 0;
                }

                const ULONGLONG now = GetTickCount64();
                if (now - lastCheck >= 1000) {
                    lastCheck = now;
                    if (now - lastUnderrunTick > 20000 && target > minTarget) {
                        target = std::max(minTarget, target - step / 2);
                        lastUnderrunTick = now;
                    }
                    // Follow the Windows default output (headphones etc.).
                    ComPtr<IMMDevice> current;
                    if (followDefault &&
                        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole,
                                                                      &current)) &&
                        DeviceId(current.Get()) != outputId)
                        restart = true;
                    UINT32 pad = 0;
                    render->GetCurrentPadding(&pad);
                    const double ms = (capPeriod / 1e4) +
                        (static_cast<double>(ring.size() / inCh - readPos) + pad) * 1000.0 / rate;
                    wchar_t text[96]{};
                    if (underruns || skips)
                        swprintf_s(text, L" (~%.0f ms, %u gaps, %u skips)", ms, underruns, skips);
                    else
                        swprintf_s(text, L" (~%.0f ms)", ms);
                    SetStatus(L"Audio: " + route + text);
                }
            }
            capture->Stop();
            render->Stop();
        } catch (const AudioError& e) {
            wchar_t text[160]{};
            swprintf_s(text, L"Audio: %hs failed (0x%08lX), retrying", e.what(),
                       static_cast<unsigned long>(e.hr));
            SetStatus(text);
            WaitForSingleObject(stopEvent_, 1000);
        }
    }

    CloseHandle(captureEvent);
    CloseHandle(renderEvent);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    running_ = false;
}

#endif // _WIN32
