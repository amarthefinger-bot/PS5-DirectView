#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AudioOutput {
    std::wstring id;   // empty = follow Windows default
    std::wstring name;
};

// Active playback endpoints (requires COM on the calling thread).
std::vector<AudioOutput> ListAudioOutputs();

// Plays the capture card's HDMI audio on the default Windows output with
// minimal delay. The card's audio endpoint is found automatically: it shares
// a USB container ID with the selected video device.
class AudioPassthrough {
public:
    AudioPassthrough() = default;
    ~AudioPassthrough();
    AudioPassthrough(const AudioPassthrough&) = delete;
    AudioPassthrough& operator=(const AudioPassthrough&) = delete;

    void Start(const std::wstring& videoSymbolicLink);
    void Stop();
    bool Running() const { return running_.load(); }
    std::wstring Status() const;
    // Empty id = Windows default output. Takes effect within one audio period.
    void SetOutput(const std::wstring& id);
    void SetVolume(float volume) { volume_ = volume; } // 0..1
    void SetMuted(bool muted) { muted_ = muted; }
    // Peak level of the last captured audio (0..1), for the UI meter.
    float Peak() const { return peak_.load(); }

private:
    void Worker(std::wstring videoSymbolicLink);
    void SetStatus(const std::wstring& value);

    mutable std::mutex statusMutex_;
    std::wstring status_ = L"Audio off";
    std::thread worker_;
    HANDLE stopEvent_ = nullptr;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::mutex outputMutex_;
    std::wstring outputId_;
    std::atomic<unsigned> outputVersion_{0};
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<float> peak_{0.0f};
};
