// Read-only network check: runs DirectView's PS5 discovery (all local subnets)
// and prints what answered. Does not pair, wake or connect.
// Build: cmake --build build --config Release --target RemotePlayProbe
#include "RemotePlayClient.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    RemotePlayClient client;
    client.Start(nullptr);
    const RpStatus initial = client.Status();
    std::printf("paired: %s  state: %ls\n", initial.paired ? "yes" : "no", RpStateName(initial.state));
    client.Discover();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (int i = 0; i < 50 && client.Status().discovering; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const RpStatus s = client.Status();
    std::printf("consoles found: %zu\n", s.found.size());
    for (const FoundConsole& c : s.found)
        std::printf("  %ls  %ls  id=%ls  fw=%ls  %s\n", c.name.c_str(), c.host.c_str(), c.id.c_str(),
                    c.firmware.c_str(), c.ready ? "ready" : "rest mode");
    client.Stop();
    return s.found.empty() ? 1 : 0;
}
