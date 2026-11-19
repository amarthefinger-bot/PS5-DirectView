#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
#endif

#ifdef _WIN32
using AudioStringT = std::wstring;
#else
using AudioStringT = std::string;
#endif

struct AudioOutput {
    AudioStringT id;   // empty = follow OS default
    AudioStringT name;
};

std::vector<AudioOutput> ListAudioOutputs();

class AudioPassthrough {
public:
    AudioPassthrough() = default;
    ~AudioPassthrough();
    AudioPassthrough(const AudioPassthrough&) = delete;
    AudioPassthrough& operator=(const AudioPassthrough&) = delete;

    void Start(const AudioStringT& videoSymbolicLink);
    void Stop();
    bool Running() const { return running_.load(); }
    AudioStringT Status() const;
    void SetOutput(const AudioStringT& id);
    void SetVolume(float volume) { volume_ = volume; } // 0..1
    void SetMuted(bool muted) { muted_ = muted; }
    float Peak() const { return peak_.load(); }

private:
    void Worker(AudioStringT videoSymbolicLink);
    void SetStatus(const AudioStringT& value);

    mutable std::mutex statusMutex_;
#ifdef _WIN32
    AudioStringT status_ = L"Audio off";
#else
    AudioStringT status_ = "Audio off";
#endif
    std::thread worker_;
#ifdef _WIN32
    HANDLE stopEvent_ = nullptr;
#endif
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::mutex outputMutex_;
    AudioStringT outputId_;
    std::atomic<unsigned> outputVersion_{0};
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<float> peak_{0.0f};
#ifndef _WIN32
    struct Impl;
    std::unique_ptr<Impl> impl_;
#endif
};
