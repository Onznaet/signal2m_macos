// Native, headless macOS-only privacy/lifetime probes. No editor or DAW is run.
// The HTTP and silent-TLS fixtures listen only on 127.0.0.1 with ephemeral ports.
#include "UpdateChecker.h"
#import <Foundation/Foundation.h>

#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace signal2m::update_detail
{
std::uint64_t macUpdateActiveContexts() noexcept;
std::uint64_t macUpdateActiveCallbacks() noexcept;
std::uint64_t macUpdateTeardownFallbacks() noexcept;
bool macUpdateImagePermanentlyPinned() noexcept;
std::uint64_t macUpdateTemporaryImagePins() noexcept;
std::uint64_t macUpdateDelegateClasses() noexcept;
}

namespace
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace detail = signal2m::update_detail;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void waitUntil(Predicate predicate, std::chrono::milliseconds budget = 1500ms)
{
    const auto deadline = Clock::now() + budget;
    while (!predicate())
    {
        require(Clock::now() < deadline, "Fixture condition did not become true within its test deadline");
        std::this_thread::sleep_for(2ms);
    }
}

bool sendAll(int socket, const std::string& bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto sent = send(socket, bytes.data() + offset, bytes.size() - offset, 0);
        if (sent <= 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

class Fixture
{
public:
    explicit Fixture(bool silentTls = false) : tls(silentTls)
    {
        listening = socket(AF_INET, SOCK_STREAM, 0);
        require(listening >= 0, "Cannot create loopback fixture socket");
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listening, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || listen(listening, 8) != 0)
        { close(listening); listening = -1; throw std::runtime_error("Cannot bind/listen on loopback fixture"); }
        socklen_t size = sizeof(address);
        if (getsockname(listening, reinterpret_cast<sockaddr*>(&address), &size) != 0)
        { close(listening); listening = -1; throw std::runtime_error("Cannot determine loopback fixture port"); }
        port = ntohs(address.sin_port);
        listener = std::thread([this]
        {
            while (!stopped.load())
            {
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(listening, &readable);
                timeval interval { 0, 20000 };
                if (select(listening + 1, &readable, nullptr, nullptr, &interval) <= 0) continue;
                const int client = accept(listening, nullptr, nullptr);
                if (client < 0) continue;
                ++accepted;
                clients.emplace_back([this, client]
                {
                    timeval interval { 0, 200000 };
                    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &interval, sizeof(interval));
                    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &interval, sizeof(interval));
                    const int suppressSigPipe = 1;
                    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &suppressSigPipe, sizeof(suppressSigPipe));
                    handle(client);
                    shutdown(client, SHUT_RDWR);
                    close(client);
                });
            }
        });
    }

    ~Fixture()
    {
        stopped.store(true);
        if (listener.joinable()) listener.join();
        if (listening >= 0) close(listening);
        for (auto& client : clients) if (client.joinable()) client.join();
    }

    juce::String url(const char* path = "/valid") const
    {
        return juce::String(tls ? "https://127.0.0.1:" : "http://127.0.0.1:") + juce::String(port) + path;
    }

    std::atomic<unsigned> accepted { 0 }, clientHelloBytes { 0 }, authRequests { 0 }, cookieTargets { 0 }, privateHeaders { 0 };

private:
    void reply(int socket, const std::string& body, const std::string& headers = {}, int code = 200)
    {
        sendAll(socket, "HTTP/1.1 " + std::to_string(code) + " Fixture\r\nContent-Type: text/csv; charset=utf-8\r\n"
                        "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n" + headers + "\r\n" + body);
    }

    void handle(int socket)
    {
        std::string request;
        char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192 && !stopped.load())
        {
            const auto count = recv(socket, buffer, sizeof(buffer), 0);
            if (count <= 0) return;
            if (tls)
            {
                clientHelloBytes += static_cast<unsigned>(count);
                while (!stopped.load()) std::this_thread::sleep_for(2ms);
                return; // Deliberately never completes TLS negotiation.
            }
            request.append(buffer, static_cast<std::size_t>(count));
        }
        std::string lower = request;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value)
        { return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : static_cast<char>(value); });
        if (lower.find("\r\ncookie:") != std::string::npos || lower.find("\r\nauthorization:") != std::string::npos
            || lower.find("\r\nproxy-authorization:") != std::string::npos) ++privateHeaders;
        const auto first = request.find(' '), last = request.find(' ', first + 1);
        if (first == std::string::npos || last == std::string::npos) return;
        const auto path = request.substr(first + 1, last - first - 1);
        const std::string body = "0.4,https://example.test/from-B1\r\n";
        if (path == "/cookie") reply(socket, "", "Location: /cookie-target\r\nSet-Cookie: lmz_fixture=private; Path=/\r\n", 302);
        else if (path == "/cookie-target") { ++cookieTargets; reply(socket, body); }
        else if (path == "/auth") { ++authRequests; reply(socket, "", "WWW-Authenticate: Basic realm=\"LMZ fixture\"\r\n", 401); }
        else if (path == "/stall") while (!stopped.load()) std::this_thread::sleep_for(2ms);
        else reply(socket, body);
    }

    const bool tls;
    int listening = -1;
    unsigned short port = 0;
    std::atomic<bool> stopped { false };
    std::thread listener;
    std::vector<std::thread> clients;
};

void requireQuiescent()
{
    require(detail::macUpdateActiveContexts() == 0, "A native callback context survived a normal fetch return");
    require(detail::macUpdateActiveCallbacks() == 0, "A native callback survived fetch return");
    require(detail::macUpdateTeardownFallbacks() == 0, "Normal native teardown unexpectedly required quarantine");
    require(!detail::macUpdateImagePermanentlyPinned(), "Normal native teardown permanently pinned the plugin image");
    require(detail::macUpdateTemporaryImagePins() == 0, "A native temporary loader reference survived fetch return");
    require(detail::macUpdateDelegateClasses() == 0, "A runtime delegate class survived normal quiescence");
}

detail::FetchResponse fetch(const juce::String& endpoint, std::chrono::milliseconds budget = 1500ms)
{
    detail::RequestControl control(Clock::now() + budget);
    auto response = detail::fetchHttp(endpoint, control);
    requireQuiescent();
    return response;
}

void privacyAndRepeatedQuiescence()
{
    Fixture fixture;
    for (unsigned i = 0; i < 12; ++i)
    {
        const auto response = fetch(fixture.url());
        require(response.success && response.statusCode == 200 && response.stage == "complete", "Repeated native HTTP fixture fetch must complete");
    }
    const auto cookies = fetch(fixture.url("/cookie"));
    require(cookies.success && fixture.cookieTargets == 1, "Cookie-setting redirect must be followed anonymously");
    const auto denied = fetch(fixture.url("/auth"));
    require(!denied.success && fixture.authRequests == 1, "Authentication must fail without a credential retry");
    require(fixture.privateHeaders == 0, "Fixture observed cookies or user/proxy authorization headers");
    require(fixture.accepted == 15, "Exactly one GET per explicit fetch and redirect must be made");
    std::this_thread::sleep_for(20ms);
    requireQuiescent();
}

void cancelAndDestroy(bool tls)
{
    Fixture fixture(tls);
    for (unsigned i = 0; i < 6; ++i)
    {
        detail::CheckerOptions options;
        options.endpoint = fixture.url(tls ? "/" : "/stall");
        auto checker = std::make_unique<signal2m::UpdateChecker>("0.3.1", options);
        const auto accepted = fixture.accepted.load();
        const auto helloBytes = fixture.clientHelloBytes.load();
        require(checker->checkForUpdates(), "Destruction fixture must start exactly one native request");
        waitUntil([&] { return fixture.accepted > accepted; });
        if (tls) waitUntil([&] { return fixture.clientHelloBytes > helloBytes; });
        const auto start = Clock::now();
        checker.reset();
        require(Clock::now() - start < 650ms, "Cancellation and native teardown exceeded their finite grace period");
        requireQuiescent();
    }
    const auto requests = fixture.accepted.load();
    std::this_thread::sleep_for(30ms);
    requireQuiescent();
    require(fixture.accepted == requests, "Native requests resumed after checker destruction");
}

void tlsDeadline(const juce::String& endpoint)
{
    const auto start = Clock::now();
    const auto response = fetch(endpoint, 150ms);
    require(!response.success, "A silent TLS peer must never produce a successful response");
    require(Clock::now() - start < 650ms, "TLS negotiation escaped the absolute deadline and native cleanup budget");
}

void invalidAndPreCancelled()
{
    for (const char* endpoint : { "file:///tmp/lmz-update", "https://user:secret@example.test/", "http://example.test/update" })
        require(!fetch(endpoint).success, "Unsupported/credentialed/non-loopback plaintext URL must fail before a request");
    detail::RequestControl cancelled(Clock::now() + 1s);
    cancelled.cancel();
    const auto response = detail::fetchHttp("https://example.test/", cancelled);
    require(!response.success && response.nativeError != 0, "Pre-cancelled request must fail without creating callbacks");
    requireQuiescent();
}

void externalFixture(const std::string& base)
{
    for (const char* path : { "/valid", "/redirect/3", "/body-at-cap" })
    {
        const auto response = fetch(juce::String::fromUTF8((base + path).c_str()));
        require(response.success && response.body.size() <= detail::maximumBodyBytes, "External fixture success/body-cap check failed");
    }
    for (const char* path : { "/redirect/4", "/oversize-no-length", "/truncated" })
    {
        const auto response = fetch(juce::String::fromUTF8((base + path).c_str()));
        require(!response.success && response.body.size() <= detail::maximumBodyBytes, "External redirect/body/truncation rejection failed");
    }
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    std::string base, tls, untrustedTls;
    for (int i = 1; i < argc; i += 2)
    {
        if (i + 1 == argc) { std::cerr << "Missing option value\n"; return 2; }
        const std::string option = argv[i];
        if (option == "--base") base = argv[i + 1];
        else if (option == "--tls") tls = argv[i + 1];
        else if (option == "--untrusted-tls") untrustedTls = argv[i + 1];
        else { std::cerr << "Unknown option\n"; return 2; }
    }
    // A direct main-thread caller must be rejected; all real probes below run
    // on an ordinary worker, as production UpdateChecker does.
    detail::RequestControl mainControl(Clock::now() + 1s);
    if (detail::fetchHttp("https://example.test/", mainControl).stage != "background-thread-required")
    { std::cerr << "FAIL main-thread guard\n"; return 1; }

    int failed = 0;
    std::thread worker([&]
    {
        const auto run = [&](const char* name, const std::function<void()>& test)
        {
            try { test(); std::cout << "PASS " << name << '\n'; }
            catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
        };
        run("invalid URLs, pre-cancel and zero native contexts", invalidAndPreCancelled);
        run("anonymous cookies/auth and repeated quiescence", privacyAndRepeatedQuiescence);
        run("HTTP cancellation/destruction quiescence", [] { cancelAndDestroy(false); });
        run("TLS handshake cancellation/destruction quiescence", [] { cancelAndDestroy(true); });
        run("silent TLS absolute deadline", [&] { Fixture peer(true); tlsDeadline(peer.url("/")); require(peer.clientHelloBytes > 0, "TLS fixture must observe an actual ClientHello"); });
        if (!base.empty()) run("external HTTP redirect/body/truncation fixture", [&] { externalFixture(base); });
        if (!tls.empty()) run("external silent TLS deadline", [&] { tlsDeadline(juce::String::fromUTF8(tls.c_str())); });
        if (!untrustedTls.empty()) run("untrusted TLS rejection", [&]
        {
            const auto response = fetch(juce::String::fromUTF8(untrustedTls.c_str()));
            require(!response.success && response.secureFailureFlags != 0, "Untrusted TLS must fail OS trust evaluation");
        });
    });
    worker.join();
    std::cout << "Native contexts=" << detail::macUpdateActiveContexts() << " callbacks=" << detail::macUpdateActiveCallbacks()
              << " teardownFallbacks=" << detail::macUpdateTeardownFallbacks() << " permanentlyPinned=" << detail::macUpdateImagePermanentlyPinned() << '\n';
    return failed == 0 ? 0 : 1;
}
