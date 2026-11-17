#pragma once

#include "ControllerState.h"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Input-only PS5 Remote Play connection (LAN), built on the vendored
// chiaki-ng protocol core. The session is opened with Remote Play audio and
// video disabled: the console still runs a minimal 360p stream (the protocol
// requires one), but every video/audio packet is dropped by the transport
// before decryption, FEC or decoding. DirectView keeps using HDMI capture.

enum class RpState {
    NotPaired,     // no stored pairing
    Disconnected,  // paired, idle
    Pairing,
    Searching,     // locating the console on the network
    Waking,        // console in rest mode: wake-up sent, waiting
    Connecting,
    Connected,
    Reconnecting,  // waiting before an automatic retry
};

struct FoundConsole {
    std::wstring name, host, id, firmware;
    bool ps5 = false;
    bool ready = false; // false = rest mode
};

struct RpStatus {
    RpState state = RpState::NotPaired;
    bool paired = false;
    bool error = false;           // message describes a failure
    std::wstring message;         // last progress / error text for the UI
    std::wstring consoleName, host;
    bool discovering = false;
    std::vector<FoundConsole> found;
    double rttMs = 0.0;           // measured by the Remote Play network test
    unsigned mtu = 0;
    ULONGLONG connectedSinceTick = 0;
    // Controller submission statistics (input thread -> Remote Play sender).
    std::uint64_t padsSubmitted = 0;
    double submitAvgUs = 0.0, submitMaxUs = 0.0;
};

const wchar_t* RpStateName(RpState state);

class RemotePlayClient {
public:
    // Posted to the notify window whenever RpStatus changes.
    static constexpr UINT kStatusMessage = WM_APP + 41;

    RemotePlayClient();
    ~RemotePlayClient();
    RemotePlayClient(const RemotePlayClient&) = delete;
    RemotePlayClient& operator=(const RemotePlayClient&) = delete;

    void Start(HWND notifyWindow);
    void Stop(); // disconnects (releasing all buttons) and joins all threads

    void Discover();
    // host: IP/hostname; account: PSN account ID (decimal or 12-char base64);
    // pin: 8-digit code from PS5 Settings > System > Remote Play > Pair Device.
    void Pair(const std::wstring& host, const std::wstring& account, const std::wstring& pin);
    void Forget();
    void Connect();
    void Disconnect();
    void SetAutoReconnect(bool enabled);

    // Thread-safe, non-blocking (one mutex + the protocol library's queue).
    // Called from the input thread for every controller change.
    void SubmitPad(const PadState& pad);

    RpStatus Status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
