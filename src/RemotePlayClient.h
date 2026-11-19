#pragma once

#include "ControllerState.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <windows.h>
#endif

#ifdef _WIN32
using RpStringT = std::wstring;
#else
using RpStringT = std::string;
#endif

enum class RpState {
    NotPaired,
    Disconnected,
    Pairing,
    Searching,
    Waking,
    Connecting,
    Connected,
    Reconnecting,
};

struct FoundConsole {
    RpStringT name, host, id, firmware;
    bool ps5 = false;
    bool ready = false;
};

struct RpStatus {
    RpState state = RpState::NotPaired;
    bool paired = false;
    bool error = false;
    RpStringT message;
    RpStringT consoleName, host;
    bool discovering = false;
    std::vector<FoundConsole> found;
    double rttMs = 0.0;
    unsigned mtu = 0;
#ifdef _WIN32
    ULONGLONG connectedSinceTick = 0;
#else
    std::uint64_t connectedSinceTick = 0; // mach_absolute_time ns
#endif
    std::uint64_t padsSubmitted = 0;
    double submitAvgUs = 0.0, submitMaxUs = 0.0;
};

#ifdef _WIN32
const wchar_t* RpStateName(RpState state);
#else
const char* RpStateName(RpState state);
#endif

class RemotePlayClient {
public:
#ifdef _WIN32
    static constexpr UINT kStatusMessage = WM_APP + 41;
#endif

    RemotePlayClient();
    ~RemotePlayClient();
    RemotePlayClient(const RemotePlayClient&) = delete;
    RemotePlayClient& operator=(const RemotePlayClient&) = delete;

#ifdef _WIN32
    void Start(HWND notifyWindow);
#else
    // macOS: notifier called on an arbitrary thread when RpStatus changes.
    using StatusNotifier = std::function<void()>;
    void Start(StatusNotifier onChange);
#endif
    void Stop();

    void Discover();
    void Pair(const RpStringT& host, const RpStringT& account, const RpStringT& pin);
    void Forget();
    void Connect();
    void Disconnect();
    void SetAutoReconnect(bool enabled);

    void SubmitPad(const PadState& pad);

    RpStatus Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
