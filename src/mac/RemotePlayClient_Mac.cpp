#include "../RemotePlayClient.h"

#ifndef _WIN32

#include "../SecureStore.h"

extern "C" {
#include <chiaki/base64.h>
#include <chiaki/common.h>
#include <chiaki/controller.h>
#include <chiaki/discovery.h>
#include <chiaki/discoveryservice.h>
#include <chiaki/log.h>
#include <chiaki/regist.h>
#include <chiaki/session.h>
}

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#include <cctype>
#include <cstdio>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace {

constexpr char kPairingValue[] = "RemotePlayPairing";
constexpr std::uint32_t kPairingMagic = 0x31505644; // "DVP1"

std::string Trim(std::string s) {
    const auto issp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back())) s.pop_back();
    return s;
}

void SecureZero(void* p, size_t n) {
    volatile unsigned char* v = static_cast<volatile unsigned char*>(p);
    while (n--) *v++ = 0;
}

std::uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Pairing {
    std::uint32_t target = CHIAKI_TARGET_PS5_1;
    char registKey[CHIAKI_SESSION_AUTH_SIZE]{};
    std::uint8_t morning[0x10]{};
    std::string host;
    std::string name;
    std::string id;

    std::vector<std::uint8_t> Serialize() const {
        std::vector<std::uint8_t> out;
        auto put = [&](const void* p, size_t n) {
            const auto* b = static_cast<const std::uint8_t*>(p);
            out.insert(out.end(), b, b + n);
        };
        auto putStr = [&](const std::string& s) {
            const std::uint16_t n = static_cast<std::uint16_t>(std::min<size_t>(s.size(), 0xFFFF));
            put(&n, 2);
            put(s.data(), n);
        };
        put(&kPairingMagic, 4);
        put(&target, 4);
        put(registKey, sizeof(registKey));
        put(morning, sizeof(morning));
        putStr(host);
        putStr(name);
        putStr(id);
        return out;
    }

    static std::optional<Pairing> Parse(const std::vector<std::uint8_t>& in) {
        Pairing p;
        size_t pos = 0;
        auto get = [&](void* dst, size_t n) {
            if (pos + n > in.size()) return false;
            std::memcpy(dst, in.data() + pos, n);
            pos += n;
            return true;
        };
        auto getStr = [&](std::string& s) {
            std::uint16_t n = 0;
            if (!get(&n, 2) || pos + n > in.size()) return false;
            s.assign(reinterpret_cast<const char*>(in.data() + pos), n);
            pos += n;
            return true;
        };
        std::uint32_t magic = 0;
        if (!get(&magic, 4) || magic != kPairingMagic) return std::nullopt;
        if (!get(&p.target, 4) || !get(p.registKey, sizeof(p.registKey)) ||
            !get(p.morning, sizeof(p.morning)) || !getStr(p.host) || !getStr(p.name) ||
            !getStr(p.id))
            return std::nullopt;
        return p;
    }

    std::uint64_t WakeCredential() const {
        std::string hex(registKey, strnlen(registKey, sizeof(registKey)));
        if (hex.empty() || hex.size() > 16) return 0;
        return std::strtoull(hex.c_str(), nullptr, 16);
    }
};

std::string QuitText(ChiakiQuitReason reason, const char* remote) {
    std::string text;
    switch (reason) {
        case CHIAKI_QUIT_REASON_STOPPED: text = "Disconnected."; break;
        case CHIAKI_QUIT_REASON_SESSION_REQUEST_CONNECTION_REFUSED:
            text = "The PS5 refused the connection. Check Settings \xe2\x80\xba System \xe2\x80\xba Remote Play \xe2\x80\xba Enable Remote Play.";
            break;
        case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE:
            text = "Remote Play is already in use. Close PS Remote Play / chiaki on other devices.";
            break;
        case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_CRASH:
            text = "Remote Play crashed on the PS5. Wait a few seconds and connect again.";
            break;
        case CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH:
            text = "This PS5 firmware uses a Remote Play version the built-in backend doesn't support.";
            break;
        case CHIAKI_QUIT_REASON_SESSION_REQUEST_UNKNOWN:
            text = "The PS5 rejected the session. If you re-linked devices on the PS5, pair DirectView again.";
            break;
        case CHIAKI_QUIT_REASON_CTRL_CONNECT_FAILED:
        case CHIAKI_QUIT_REASON_CTRL_CONNECTION_REFUSED:
            text = "Could not open the Remote Play control connection (TCP 9295). Is the PS5 on and on this network?";
            break;
        case CHIAKI_QUIT_REASON_CTRL_UNKNOWN:
            text = "The Remote Play control connection failed.";
            break;
        case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_DISCONNECTED:
            text = "The PS5 ended the Remote Play session.";
            break;
        case CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN:
            text = "The PS5 is shutting down or entering rest mode.";
            break;
        case CHIAKI_QUIT_REASON_STREAM_CONNECTION_UNKNOWN:
            text = "The Remote Play stream connection failed (UDP 9296). Check firewall/network.";
            break;
        default:
            text = chiaki_quit_reason_string(reason);
            break;
    }
    if (remote && *remote) { text += " ("; text += remote; text += ")"; }
    return text;
}

bool QuitIsFatal(ChiakiQuitReason reason) {
    return reason == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_VERSION_MISMATCH ||
           reason == CHIAKI_QUIT_REASON_PSN_REGIST_FAILED;
}

std::vector<sockaddr_storage> DirectedBroadcasts() {
    std::vector<sockaddr_storage> out;
    struct ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0 || !ifs) return out;
    for (struct ifaddrs* a = ifs; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        if (!a->ifa_netmask) continue;
        const sockaddr_in* ip = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
        const sockaddr_in* nm = reinterpret_cast<const sockaddr_in*>(a->ifa_netmask);
        const std::uint32_t ipN = ntohl(ip->sin_addr.s_addr);
        const std::uint32_t maskN = ntohl(nm->sin_addr.s_addr);
        if (maskN == 0 || maskN == 0xFFFFFFFFu) continue;
        sockaddr_in bcast{};
        bcast.sin_family = AF_INET;
        bcast.sin_addr.s_addr = htonl(ipN | ~maskN);
        bcast.sin_port = 0;
        sockaddr_storage ss{};
        std::memcpy(&ss, &bcast, sizeof(bcast));
        out.push_back(ss);
    }
    freeifaddrs(ifs);
    return out;
}

double ChronoUs(std::chrono::steady_clock::time_point a,
                std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

} // namespace

const char* RpStateName(RpState state) {
    switch (state) {
        case RpState::NotPaired: return "Not paired";
        case RpState::Disconnected: return "Disconnected";
        case RpState::Pairing: return "Pairing";
        case RpState::Searching: return "Searching";
        case RpState::Waking: return "Waking PS5";
        case RpState::Connecting: return "Connecting";
        case RpState::Connected: return "Connected";
        case RpState::Reconnecting: return "Reconnecting";
    }
    return "";
}

struct RemotePlayClient::Impl {
    RemotePlayClient::StatusNotifier notify;
    ChiakiLog log{};
    bool libReady = false;

    mutable std::mutex statusMutex;
    RpStatus status;
    std::optional<Pairing> pairing;

    enum class Cmd { Pair, Forget, Connect };
    struct Command { Cmd cmd; std::string a, b, c; };
    std::mutex cmdMutex;
    std::condition_variable cmdCv;
    std::deque<Command> queue;
    bool stopping = false;
    bool wantConnected = false;
    bool autoReconnect = true;

    bool evConnected = false, evQuit = false, evLoginPin = false;
    ChiakiQuitReason quitReason = CHIAKI_QUIT_REASON_NONE;
    std::string quitRemote;

    std::thread worker;
    std::thread discoverThread;
    std::atomic<bool> discovering{false};

    std::mutex padMutex;
    ChiakiSession* activeSession = nullptr;
    PadState lastPad;
    std::uint64_t padCount = 0;
    double submitSumUs = 0, submitMaxUs = 0;
    std::uint64_t submitSamples = 0;
    std::uint64_t windowStart = 0;
    double publishedAvgUs = 0, publishedMaxUs = 0;

    void Notify() { if (notify) notify(); }

    void SetState(RpState s, std::string message = {}, bool error = false) {
        {
            std::lock_guard lock(statusMutex);
            status.state = s;
            status.message = std::move(message);
            status.error = error;
            if (s != RpState::Connected) status.connectedSinceTick = 0;
        }
        Notify();
    }

    void SetMessage(std::string message, bool error) {
        {
            std::lock_guard lock(statusMutex);
            status.message = std::move(message);
            status.error = error;
        }
        Notify();
    }

    bool WantConnected() {
        std::lock_guard lock(cmdMutex);
        return wantConnected && !stopping;
    }

    RpState IdleState() {
        std::lock_guard lock(statusMutex);
        return pairing ? RpState::Disconnected : RpState::NotPaired;
    }

    void PublishPairing() {
        std::lock_guard lock(statusMutex);
        status.paired = pairing.has_value();
        status.consoleName = pairing ? pairing->name : "";
        status.host = pairing ? pairing->host : "";
    }

    template <class Pred>
    bool WaitFor(std::unique_lock<std::mutex>& lock, std::chrono::milliseconds timeout, Pred pred) {
        cmdCv.wait_for(lock, timeout, [&] { return stopping || pred(); });
        return !stopping;
    }

    static void LogCb(ChiakiLogLevel level, const char* msg, void*) {
        const char* tag = level == CHIAKI_LOG_ERROR ? "E" :
                          level == CHIAKI_LOG_WARNING ? "W" : "I";
        std::fprintf(stderr, "[chiaki] %s %s\n", tag, msg);
    }

    struct ProbeCtx {
        std::mutex m;
        std::vector<FoundConsole> found;
    };

    static void DiscoveryCb(ChiakiDiscoveryHost* hosts, size_t count, void* user) {
        auto* ctx = static_cast<ProbeCtx*>(user);
        std::lock_guard lock(ctx->m);
        ctx->found.clear();
        for (size_t i = 0; i < count; ++i) {
            const ChiakiDiscoveryHost& h = hosts[i];
            FoundConsole c;
            c.host = h.host_addr ? h.host_addr : "";
            c.name = h.host_name ? h.host_name : "";
            c.id   = h.host_id   ? h.host_id   : "";
            c.firmware = h.system_version ? h.system_version : "";
            c.ps5 = chiaki_discovery_host_is_ps5(const_cast<ChiakiDiscoveryHost*>(&h));
            c.ready = h.state == CHIAKI_DISCOVERY_HOST_STATE_READY;
            ctx->found.push_back(std::move(c));
        }
    }

    std::vector<FoundConsole> Probe(const std::string& host, int timeoutMs,
                                    const std::function<bool(const std::vector<FoundConsole>&)>& done,
                                    bool forConnect = false) {
        ProbeCtx ctx;
        sockaddr_storage send{};
        auto* in = reinterpret_cast<sockaddr_in*>(&send);
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = INADDR_BROADCAST;
        std::vector<sockaddr_storage> broadcasts;
        ChiakiDiscoveryServiceOptions options{};
        options.hosts_max = 16;
        options.host_drop_pings = 3;
        options.ping_ms = 500;
        options.ping_initial_ms = 0;
        options.send_addr = &send;
        options.send_addr_size = sizeof(sockaddr_in);
        options.cb = &Impl::DiscoveryCb;
        options.cb_user = &ctx;
        std::string hostCopy = host;
        if (!host.empty()) {
            options.send_host = hostCopy.data();
        } else {
            broadcasts = DirectedBroadcasts();
            options.broadcast_addrs = broadcasts.empty() ? nullptr : broadcasts.data();
            options.broadcast_num = broadcasts.size();
        }
        ChiakiDiscoveryService service;
        if (chiaki_discovery_service_init(&service, &options, &log) != CHIAKI_ERR_SUCCESS)
            return {};
        std::vector<FoundConsole> result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard lock(ctx.m);
                result = ctx.found;
            }
            if (done && done(result)) break;
            {
                std::lock_guard lock(cmdMutex);
                if (stopping || (forConnect && !wantConnected)) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        chiaki_discovery_service_fini(&service);
        std::lock_guard lock(ctx.m);
        return ctx.found;
    }

    void RunDiscover() {
        {
            std::lock_guard lock(statusMutex);
            status.discovering = true;
        }
        Notify();
        auto found = Probe({}, 2500, nullptr);
        std::erase_if(found, [](const FoundConsole& c) { return !c.ps5; });
        {
            std::lock_guard lock(statusMutex);
            status.found = found;
            status.discovering = false;
        }
        discovering = false;
        Notify();
    }

    std::optional<FoundConsole> Locate(Pairing& p) {
        const std::string id = p.id;
        auto matches = [&](const FoundConsole& c) { return c.ps5 && (id.empty() || c.id == id); };
        auto hasMatch = [&](const std::vector<FoundConsole>& v) {
            return std::any_of(v.begin(), v.end(), matches);
        };
        std::vector<std::string> attempts;
        if (!p.host.empty()) attempts.push_back(p.host);
        attempts.emplace_back();
        for (const std::string& host : attempts) {
            if (!WantConnected()) break;
            auto found = Probe(host, host.empty() ? 3000 : 2000, hasMatch, true);
            for (auto& c : found)
                if (matches(c)) return c;
        }
        return std::nullopt;
    }

    void RunPair(const std::string& hostIn, const std::string& accountIn, const std::string& pinIn) {
        SetState(RpState::Pairing, "Checking the PS5\xe2\x80\xa6");
        const std::string host = Trim(hostIn);
        const std::string account = Trim(accountIn);
        std::string pinText;
        for (char c : pinIn) if (c >= '0' && c <= '9') pinText += c;

        auto fail = [&](std::string text) { SetState(IdleState(), std::move(text), true); };
        if (host.empty()) return fail("Enter the PS5's IP address, or press Find and pick it.");
        if (pinText.size() != 8) return fail("The PIN must be the 8 digits shown on the PS5.");

        std::uint8_t accountId[CHIAKI_PSN_ACCOUNT_ID_SIZE]{};
        bool accountOk = false;
        auto allDigits = [](const std::string& s) {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) {
                return static_cast<unsigned char>(c) < 128 && std::isdigit(static_cast<unsigned char>(c));
            });
        };
        if (allDigits(account)) {
            const unsigned long long v = std::strtoull(account.c_str(), nullptr, 10);
            std::memcpy(accountId, &v, sizeof(accountId));
            accountOk = v != 0;
        } else if (!account.empty()) {
            size_t size = sizeof(accountId);
            accountOk = chiaki_base64_decode(account.c_str(), account.size(), accountId, &size) ==
                            CHIAKI_ERR_SUCCESS && size == sizeof(accountId);
        }
        if (!accountOk)
            return fail("PSN account ID not recognised. Use the 12-character code ending in '=' or the decimal ID.");

        auto found = Probe(host, 2500, [](const std::vector<FoundConsole>& v) { return !v.empty(); });
        if (found.empty())
            return fail("No PS5 answered at " + host + ". Check the IP and that the PS5 is on.");
        const FoundConsole console = found.front();
        if (!console.ps5) return fail("That console isn't a PS5.");
        if (!console.ready) return fail("The PS5 is in rest mode. Turn it on fully to pair.");

        SetState(RpState::Pairing, "Pairing with " + console.name + "\xe2\x80\xa6");
        ChiakiRegistInfo info{};
        info.target = CHIAKI_TARGET_PS5_1;
        info.host = host.c_str();
        info.broadcast = false;
        info.psn_online_id = nullptr;
        std::memcpy(info.psn_account_id, accountId, sizeof(accountId));
        info.pin = static_cast<std::uint32_t>(std::strtoul(pinText.c_str(), nullptr, 10));
        info.console_pin = 0;
        info.holepunch_info = nullptr;
        info.rudp = nullptr;

        struct RegistCtx {
            std::mutex m;
            std::condition_variable cv;
            bool done = false;
            ChiakiRegistEventType type = CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED;
            ChiakiRegisteredHost host{};
        } ctx;
        auto cb = [](ChiakiRegistEvent* e, void* user) {
            auto* c = static_cast<RegistCtx*>(user);
            std::lock_guard lock(c->m);
            c->type = e->type;
            if (e->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && e->registered_host)
                c->host = *e->registered_host;
            c->done = true;
            c->cv.notify_all();
        };
        ChiakiRegist regist;
        if (chiaki_regist_start(&regist, &log, &info, cb, &ctx) != CHIAKI_ERR_SUCCESS)
            return fail("Could not start pairing.");
        {
            std::unique_lock lock(ctx.m);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!ctx.done && std::chrono::steady_clock::now() < deadline) {
                ctx.cv.wait_for(lock, std::chrono::milliseconds(100));
                std::lock_guard stopLock(cmdMutex);
                if (stopping) break;
            }
        }
        bool done;
        {
            std::lock_guard lock(ctx.m);
            done = ctx.done;
        }
        if (!done) chiaki_regist_stop(&regist);
        chiaki_regist_fini(&regist);
        SecureZero(accountId, sizeof(accountId));

        if (!done || ctx.type != CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS)
            return fail("Pairing failed. Check the PIN (it expires after ~5 minutes), the PSN account ID, "
                        "and that Remote Play is enabled on the PS5.");

        Pairing p;
        p.target = static_cast<std::uint32_t>(ctx.host.target ? ctx.host.target : CHIAKI_TARGET_PS5_1);
        std::memcpy(p.registKey, ctx.host.rp_regist_key, sizeof(p.registKey));
        std::memcpy(p.morning, ctx.host.rp_key, sizeof(p.morning));
        p.host = host;
        p.name = ctx.host.server_nickname[0] ? std::string(ctx.host.server_nickname, strnlen(ctx.host.server_nickname, sizeof(ctx.host.server_nickname)))
                                             : console.name;
        p.id = console.id;
        SecureZero(&ctx.host, sizeof(ctx.host));
        const auto blob = p.Serialize();
        const bool saved = secure_store::Save(kPairingValue, blob);
        {
            std::lock_guard lock(statusMutex);
            pairing = p;
        }
        PublishPairing();
        SetState(RpState::Disconnected,
                 saved ? "Paired with " + p.name + ". Connecting\xe2\x80\xa6"
                       : "Paired, but saving the pairing failed (it will be lost on exit).",
                 !saved);
        std::lock_guard lock(cmdMutex);
        wantConnected = true;
        queue.push_back({Cmd::Connect, {}, {}, {}});
    }

    static void EventCb(ChiakiEvent* e, void* user) {
        auto* self = static_cast<Impl*>(user);
        std::lock_guard lock(self->cmdMutex);
        switch (e->type) {
            case CHIAKI_EVENT_CONNECTED: self->evConnected = true; break;
            case CHIAKI_EVENT_QUIT:
                self->evQuit = true;
                self->quitReason = e->quit.reason;
                self->quitRemote = e->quit.reason_str ? e->quit.reason_str : "";
                break;
            case CHIAKI_EVENT_LOGIN_PIN_REQUEST: self->evLoginPin = true; break;
            default: break;
        }
        self->cmdCv.notify_all();
    }

    bool ConnectOnce() {
        std::optional<Pairing> p;
        {
            std::lock_guard lock(statusMutex);
            p = pairing;
        }
        if (!p) {
            SetState(RpState::NotPaired, "Pair with your PS5 first.", true);
            return false;
        }

        SetState(RpState::Searching, "Looking for " + p->name + "\xe2\x80\xa6");
        auto console = Locate(*p);
        auto cancelled = [&] {
            if (WantConnected()) return false;
            SetState(IdleState(), "Disconnected.");
            return true;
        };
        if (cancelled()) return false;
        if (!console) {
            SetState(RpState::Disconnected, "PS5 not found on the network. Is it on (or in rest mode with "
                     "network features enabled) and on the same LAN?", true);
            return true;
        }
        if (console->host != p->host) {
            p->host = console->host;
            secure_store::Save(kPairingValue, p->Serialize());
            {
                std::lock_guard lock(statusMutex);
                pairing = *p;
            }
            PublishPairing();
        }

        if (!console->ready) {
            SetState(RpState::Waking, "Waking the PS5 from rest mode\xe2\x80\xa6");
            ChiakiErrorCode err = chiaki_discovery_wakeup(&log, nullptr, p->host.c_str(),
                                                          p->WakeCredential(), true);
            if (err != CHIAKI_ERR_SUCCESS) {
                SetState(RpState::Disconnected, "Sending the wake-up failed.", true);
                return true;
            }
            auto ready = Probe(p->host, 45000, [](const std::vector<FoundConsole>& v) {
                return !v.empty() && v.front().ready;
            }, true);
            if (cancelled()) return false;
            if (ready.empty() || !ready.front().ready) {
                SetState(RpState::Disconnected, "The PS5 didn't wake up. Enable network wake-up in PS5 "
                         "power-saving settings.", true);
                return true;
            }
        }

        SetState(RpState::Connecting, "Starting input-only Remote Play\xe2\x80\xa6");
        ChiakiConnectInfo info{};
        info.ps5 = true;
        info.host = p->host.c_str();
        std::memcpy(info.regist_key, p->registKey, sizeof(info.regist_key));
        std::memcpy(info.morning, p->morning, sizeof(info.morning));
        chiaki_connect_video_profile_preset(&info.video_profile, CHIAKI_VIDEO_RESOLUTION_PRESET_360p,
                                            CHIAKI_VIDEO_FPS_PRESET_30);
        info.video_profile.codec = CHIAKI_CODEC_H264;
        info.video_profile_auto_downgrade = true;
        info.audio_video_disabled = CHIAKI_AUDIO_VIDEO_DISABLED;
        info.enable_keyboard = false;
        info.enable_dualsense = false;
        info.auto_regist = false;
        info.holepunch_session = nullptr;
        info.rudp_sock = nullptr;
        info.packet_loss_max = 0.05;
        info.enable_idr_on_fec_failure = false;

        auto session = std::make_unique<ChiakiSession>();
        std::memset(session.get(), 0, sizeof(ChiakiSession));
        {
            std::lock_guard lock(cmdMutex);
            evConnected = evQuit = evLoginPin = false;
            quitReason = CHIAKI_QUIT_REASON_NONE;
            quitRemote.clear();
        }
        ChiakiErrorCode err = chiaki_session_init(session.get(), &info, &log);
        if (err != CHIAKI_ERR_SUCCESS) {
            SetState(RpState::Disconnected, std::string("Remote Play init failed: ") + chiaki_error_string(err), true);
            return true;
        }
        chiaki_session_set_event_cb(session.get(), &Impl::EventCb, this);
        err = chiaki_session_start(session.get());
        if (err != CHIAKI_ERR_SUCCESS) {
            chiaki_session_fini(session.get());
            SetState(RpState::Disconnected, std::string("Remote Play start failed: ") + chiaki_error_string(err), true);
            return true;
        }

        bool connected = false, userStop = false, timedOut = false;
        {
            std::unique_lock lock(cmdMutex);
            const bool alive = WaitFor(lock, std::chrono::seconds(40),
                                       [&] { return evConnected || evQuit || evLoginPin || !wantConnected; });
            connected = evConnected && !evQuit;
            userStop = !alive || !wantConnected;
            timedOut = !evConnected && !evQuit && !evLoginPin && !userStop;
        }

        if (connected && !userStop) {
            {
                std::lock_guard lock(statusMutex);
                status.rttMs = session->rtt_us / 1000.0;
                status.mtu = session->mtu_in;
                status.connectedSinceTick = NowMs();
            }
            {
                std::lock_guard lock(padMutex);
                activeSession = session.get();
                padCount = 0;
                submitSumUs = submitMaxUs = publishedAvgUs = publishedMaxUs = 0;
                submitSamples = 0;
                windowStart = NowMs();
                lastPad = PadState{};
                ChiakiControllerState idle;
                chiaki_controller_state_set_idle(&idle);
                chiaki_session_set_controller_state(session.get(), &idle);
            }
            SetState(RpState::Connected, "Connected to " + p->name + " (input only).");
            {
                {
                    std::lock_guard lock(statusMutex);
                    status.connectedSinceTick = NowMs();
                }
                std::unique_lock lock(cmdMutex);
                const bool alive = WaitFor(lock, std::chrono::hours(24 * 365),
                                           [&] { return evQuit || evLoginPin || !wantConnected; });
                userStop = !alive || !wantConnected;
            }
        }

        {
            std::lock_guard lock(padMutex);
            if (activeSession) {
                ChiakiControllerState idle;
                chiaki_controller_state_set_idle(&idle);
                chiaki_session_set_controller_state(activeSession, &idle);
            }
            activeSession = nullptr;
        }
        bool quit, loginPin;
        ChiakiQuitReason reason;
        std::string remote;
        {
            std::lock_guard lock(cmdMutex);
            quit = evQuit;
            loginPin = evLoginPin;
            reason = quitReason;
            remote = quitRemote;
        }
        if (!quit) std::this_thread::sleep_for(std::chrono::milliseconds(60));
        chiaki_session_stop(session.get());
        chiaki_session_join(session.get());
        chiaki_session_fini(session.get());
        {
            std::lock_guard lock(statusMutex);
            status.rttMs = 0;
            status.mtu = 0;
        }

        if (userStop) {
            SetState(IdleState(), "Disconnected.");
            return false;
        }
        if (loginPin) {
            SetState(RpState::Disconnected, "The PS5 user has a login passcode. Turn off the passcode for "
                     "this user (Settings \xe2\x80\xba Users and Accounts \xe2\x80\xba Login Settings) to use DirectView input.", true);
            return false;
        }
        if (timedOut) {
            SetState(RpState::Disconnected, "The PS5 didn't answer the Remote Play request in time.", true);
            return true;
        }
        SetState(RpState::Disconnected, QuitText(reason, remote.c_str()),
                 chiaki_quit_reason_is_error(reason));
        return !QuitIsFatal(reason) && reason != CHIAKI_QUIT_REASON_STREAM_CONNECTION_REMOTE_SHUTDOWN;
    }

    void RunConnect() {
        int attempt = 0;
        for (;;) {
            {
                std::lock_guard lock(cmdMutex);
                if (stopping || !wantConnected) break;
            }
            const bool retryable = ConnectOnce();
            bool retry;
            {
                std::lock_guard lock(cmdMutex);
                retry = retryable && autoReconnect && wantConnected && !stopping;
            }
            if (!retry) break;
            const int seconds = std::min(10 + attempt * 5, 30);
            ++attempt;
            std::string last;
            {
                std::lock_guard lock(statusMutex);
                last = status.message;
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Reconnecting in %d s\xe2\x80\xa6 ", seconds);
            SetState(RpState::Reconnecting, std::string(buf) + last, true);
            std::unique_lock lock(cmdMutex);
            if (!WaitFor(lock, std::chrono::seconds(seconds), [&] { return !wantConnected; })) break;
        }
        std::lock_guard lock(cmdMutex);
        wantConnected = false;
    }

    void WorkerLoop() {
        for (;;) {
            Command c;
            {
                std::unique_lock lock(cmdMutex);
                cmdCv.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping) return;
                c = std::move(queue.front());
                queue.pop_front();
            }
            switch (c.cmd) {
                case Cmd::Pair: RunPair(c.a, c.b, c.c); break;
                case Cmd::Forget:
                    secure_store::Erase(kPairingValue);
                    {
                        std::lock_guard lock(statusMutex);
                        pairing.reset();
                    }
                    PublishPairing();
                    SetState(RpState::NotPaired, "Pairing removed from this Mac.");
                    break;
                case Cmd::Connect: RunConnect(); break;
            }
        }
    }
};

RemotePlayClient::RemotePlayClient() : impl_(std::make_unique<Impl>()) {}

RemotePlayClient::~RemotePlayClient() { Stop(); }

void RemotePlayClient::Start(StatusNotifier onChange) {
    Impl& d = *impl_;
    d.notify = std::move(onChange);
    chiaki_log_init(&d.log, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO, &Impl::LogCb, nullptr);
    d.libReady = chiaki_lib_init() == CHIAKI_ERR_SUCCESS;
    if (auto blob = secure_store::Load(kPairingValue)) {
        d.pairing = Pairing::Parse(*blob);
        SecureZero(blob->data(), blob->size());
    }
    d.PublishPairing();
    d.SetState(d.pairing ? RpState::Disconnected : RpState::NotPaired,
               d.libReady ? "" : "Remote Play backend failed to initialise.", !d.libReady);
    if (d.libReady) d.worker = std::thread([&d] { d.WorkerLoop(); });
}

void RemotePlayClient::Stop() {
    Impl& d = *impl_;
    {
        std::lock_guard lock(d.cmdMutex);
        d.stopping = true;
        d.wantConnected = false;
        d.queue.clear();
    }
    d.cmdCv.notify_all();
    if (d.worker.joinable()) d.worker.join();
    if (d.discoverThread.joinable()) d.discoverThread.join();
    d.notify = nullptr;
}

void RemotePlayClient::Discover() {
    Impl& d = *impl_;
    if (!d.libReady || d.discovering.exchange(true)) return;
    if (d.discoverThread.joinable()) d.discoverThread.join();
    d.discoverThread = std::thread([&d] { d.RunDiscover(); });
}

void RemotePlayClient::Pair(const std::string& host, const std::string& account, const std::string& pin) {
    Impl& d = *impl_;
    {
        std::lock_guard lock(d.cmdMutex);
        if (d.wantConnected) d.wantConnected = false;
        d.queue.push_back({Impl::Cmd::Pair, host, account, pin});
    }
    d.cmdCv.notify_all();
}

void RemotePlayClient::Forget() {
    Impl& d = *impl_;
    {
        std::lock_guard lock(d.cmdMutex);
        d.wantConnected = false;
        d.queue.push_back({Impl::Cmd::Forget, {}, {}, {}});
    }
    d.cmdCv.notify_all();
}

void RemotePlayClient::Connect() {
    Impl& d = *impl_;
    {
        std::lock_guard lock(d.cmdMutex);
        if (d.wantConnected) return;
        d.wantConnected = true;
        d.queue.push_back({Impl::Cmd::Connect, {}, {}, {}});
    }
    d.cmdCv.notify_all();
}

void RemotePlayClient::Disconnect() {
    Impl& d = *impl_;
    {
        std::lock_guard lock(d.cmdMutex);
        d.wantConnected = false;
        std::erase_if(d.queue, [](const Impl::Command& c) { return c.cmd == Impl::Cmd::Connect; });
    }
    d.cmdCv.notify_all();
}

void RemotePlayClient::SetAutoReconnect(bool enabled) {
    std::lock_guard lock(impl_->cmdMutex);
    impl_->autoReconnect = enabled;
}

void RemotePlayClient::SubmitPad(const PadState& pad) {
    Impl& d = *impl_;
    std::lock_guard lock(d.padMutex);
    d.lastPad = pad;
    if (!d.activeSession) return;

    const auto t0 = std::chrono::steady_clock::now();
    ChiakiControllerState s;
    chiaki_controller_state_set_idle(&s);
    static constexpr std::uint32_t kMap[kPadButtonCount] = {
        CHIAKI_CONTROLLER_BUTTON_CROSS, CHIAKI_CONTROLLER_BUTTON_MOON,
        CHIAKI_CONTROLLER_BUTTON_BOX, CHIAKI_CONTROLLER_BUTTON_PYRAMID,
        CHIAKI_CONTROLLER_BUTTON_DPAD_UP, CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN,
        CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT, CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT,
        CHIAKI_CONTROLLER_BUTTON_L1, CHIAKI_CONTROLLER_BUTTON_R1,
        CHIAKI_CONTROLLER_BUTTON_L3, CHIAKI_CONTROLLER_BUTTON_R3,
        CHIAKI_CONTROLLER_BUTTON_OPTIONS, CHIAKI_CONTROLLER_BUTTON_SHARE,
        CHIAKI_CONTROLLER_BUTTON_TOUCHPAD, CHIAKI_CONTROLLER_BUTTON_PS,
    };
    for (int i = 0; i < kPadButtonCount; ++i)
        if (pad.Pressed(static_cast<PadButton>(i))) s.buttons |= kMap[i];
    s.l2_state = pad.l2;
    s.r2_state = pad.r2;
    s.left_x = pad.lx;
    s.left_y = pad.ly;
    s.right_x = pad.rx;
    s.right_y = pad.ry;
    chiaki_session_set_controller_state(d.activeSession, &s);
    const auto t1 = std::chrono::steady_clock::now();
    const double us = ChronoUs(t0, t1);
    ++d.padCount;
    d.submitSumUs += us;
    ++d.submitSamples;
    d.submitMaxUs = std::max(d.submitMaxUs, us);
    const std::uint64_t now = NowMs();
    if (now - d.windowStart >= 1000) {
        d.publishedAvgUs = d.submitSamples ? d.submitSumUs / static_cast<double>(d.submitSamples) : 0.0;
        d.publishedMaxUs = d.submitMaxUs;
        d.submitSumUs = d.submitMaxUs = 0;
        d.submitSamples = 0;
        d.windowStart = now;
    }
}

RpStatus RemotePlayClient::Status() const {
    Impl& d = *impl_;
    RpStatus s;
    {
        std::lock_guard lock(d.statusMutex);
        s = d.status;
    }
    {
        std::lock_guard lock(d.padMutex);
        s.padsSubmitted = d.padCount;
        s.submitAvgUs = d.publishedAvgUs;
        s.submitMaxUs = d.publishedMaxUs;
    }
    return s;
}

#endif // !_WIN32
