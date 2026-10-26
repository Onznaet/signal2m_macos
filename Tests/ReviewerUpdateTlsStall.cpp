// Native transport-only connection-stall probe; no public endpoint or GUI.
#include "UpdateChecker.h"
#include <iostream>
#include <string>

int main (int argc, char** argv)
{
    std::string endpoint;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string flag (argv[i]);
        if (flag == "--tls") endpoint = argv[i + 1];
        else if (flag != "--base") return 2;
    }
    if (endpoint.empty()) return 2;
    using namespace signal2m::update_detail;
    using namespace std::chrono_literals;
    const auto started = RequestControl::Clock::now();
    RequestControl control (started + 500ms);
    const auto response = fetchHttp (juce::String (endpoint), control);
    const auto elapsed = std::chrono::duration<double, std::milli> (RequestControl::Clock::now() - started).count();
    const auto passed = ! response.success && response.nativeError == 12002
        && elapsed >= 350.0 && elapsed < 2500.0;
    std::cout << "METRIC silent_TLS_pending_handshake elapsed_ms=" << elapsed
              << " stage=" << response.stage << " native_error=" << response.nativeError << '\n';
    std::cout << (passed ? "PASS" : "FAIL")
              << " pending TLS setup obeys total deadline and native callback drain completes\n";
    return passed ? 0 : 1;
}
