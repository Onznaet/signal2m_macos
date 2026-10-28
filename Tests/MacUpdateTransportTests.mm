// Native, headless macOS-only privacy/lifetime probes. No editor or DAW is run.
// The HTTP and silent-TLS fixtures listen only on 127.0.0.1 with ephemeral ports.
#include "UpdateChecker.h"
#include "DonationLinks.h"
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
std::uint32_t macUpdateLastTeardownFlags() noexcept;
const char* macUpdateLastNetworkStage() noexcept;
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
    std::atomic<unsigned> concurrentUpdates { 0 }, concurrentDonations { 0 }, nonGetRequests { 0 };
    std::atomic<bool> releaseConcurrentUpdate { false }, releaseConcurrentDonation { false };

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
        if (request.rfind("GET ", 0) != 0) ++nonGetRequests;
        const auto path = request.substr(first + 1, last - first - 1);
        if (path == "/concurrent-update" || path == "/concurrent-donations")
        {
            const bool donation = path == "/concurrent-donations";
            if (donation) ++concurrentDonations;
            else ++concurrentUpdates;
            auto& released = donation ? releaseConcurrentDonation : releaseConcurrentUpdate;
            while (!released.load(std::memory_order_acquire) && !stopped.load()) std::this_thread::sleep_for(2ms);
            if (stopped.load()) return;
            if (donation)
                reply(socket, "\xef\xbb\xbf\"" + url(u8"/помощь?item=a,b").toStdString()
                                + "\"\r\n\"" + url("/donate-second").toStdString() + "\"\r\n");
            else reply(socket, "9.9.9," + url("/update-from-B1").toStdString() + "\r\n");
            return;
        }
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

void diagnostics(const detail::FetchResponse* response = nullptr)
{
    const auto flags = detail::macUpdateLastTeardownFlags();
    std::cerr << "TRANSPORT networkStage=" << detail::macUpdateLastNetworkStage()
              << " attempted=" << ((flags & 1U) != 0) << " session=" << ((flags & 2U) != 0)
              << " invalidated=" << ((flags & 4U) != 0) << " queueEmpty=" << ((flags & 8U) != 0)
              << " callbacksZero=" << ((flags & 16U) != 0)
              << " sessionGone=" << ((flags & 32U) != 0) << " queueGone=" << ((flags & 64U) != 0)
              << " delegateGone=" << ((flags & 128U) != 0) << " networkSuccess=" << ((flags & 256U) != 0)
              << " quiescent=" << ((flags & 512U) != 0) << " forced=" << ((flags & 1024U) != 0)
              << " delegateReleased=" << ((flags & 2048U) != 0)
              << " contexts=" << detail::macUpdateActiveContexts() << " callbacks=" << detail::macUpdateActiveCallbacks()
              << " classes=" << detail::macUpdateDelegateClasses() << " temporaryPins=" << detail::macUpdateTemporaryImagePins()
              << " fallbacks=" << detail::macUpdateTeardownFallbacks();
    if (response != nullptr)
        std::cerr << " resultStage=" << response->stage << " success=" << response->success
                  << " status=" << response->statusCode << " bytes=" << response->body.size()
                  << " nativeError=" << response->nativeError;
    std::cerr << '\n';
}

void requireQuiescent()
{
    if (detail::macUpdateActiveContexts() != 0 || detail::macUpdateActiveCallbacks() != 0
        || detail::macUpdateTeardownFallbacks() != 0 || detail::macUpdateImagePermanentlyPinned()
        || detail::macUpdateTemporaryImagePins() != 0 || detail::macUpdateDelegateClasses() != 0) diagnostics();
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
    if (!response.success) diagnostics(&response);
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
        if (i == 0) diagnostics(&response);
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

void concurrentUpdaterAndDonations()
{
    enum class Completion { both, cancelDonation, cancelUpdate };
    for (const auto mode : { Completion::both, Completion::cancelDonation, Completion::cancelUpdate })
    {
        Fixture fixture;
        detail::CheckerOptions updateOptions;
        updateOptions.endpoint = fixture.url("/concurrent-update");
        updateOptions.timeout = 5s;
        detail::CheckerOptions donationOptions;
        donationOptions.endpoint = fixture.url("/concurrent-donations");
        donationOptions.timeout = 5s;
        auto updater = std::make_unique<signal2m::UpdateChecker>("0.0.0", updateOptions);
        auto donations = std::make_unique<signal2m::DonationLinks>(donationOptions);
        require(updater->checkForUpdates() && donations->refresh(), "Updater and donation loaders must start independent native workers");
        // Both request handlers hold response headers until explicitly released.
        // This proves actual overlap, rather than two sequential fast fetches.
        waitUntil([&] { return fixture.concurrentUpdates == 1 && fixture.concurrentDonations == 1; });
        require(updater->result().state == signal2m::UpdateState::checking
                && donations->result().state == signal2m::DonationState::loading,
                "Both native requests must remain active at the fixture barrier");
        require(detail::macUpdateActiveContexts() == 2 && detail::macUpdateDelegateClasses() == 2,
                "Overlapping requests must own two separate callback contexts and delegate classes");

        if (mode == Completion::cancelDonation)
        {
            const auto start = Clock::now();
            donations.reset(); // Destruction cancels and joins only this worker.
            require(Clock::now() - start < 650ms, "Concurrent donation destruction must retain bounded native cancellation");
            require(updater->result().state == signal2m::UpdateState::checking,
                    "Cancelling donations must not cancel or complete the updater request");
        }
        else if (mode == Completion::cancelUpdate)
        {
            const auto start = Clock::now();
            updater.reset();
            require(Clock::now() - start < 650ms, "Concurrent updater destruction must retain bounded native cancellation");
            require(donations->result().state == signal2m::DonationState::loading,
                    "Cancelling updates must not cancel or complete the donation request");
        }
        if (mode != Completion::both)
            require(detail::macUpdateActiveContexts() == 1 && detail::macUpdateDelegateClasses() == 1,
                    "Cancelling one request must release only its own native context and delegate class");

        if (updater != nullptr) fixture.releaseConcurrentUpdate.store(true, std::memory_order_release);
        if (donations != nullptr) fixture.releaseConcurrentDonation.store(true, std::memory_order_release);
        if (updater != nullptr)
        {
            waitUntil([&] { return updater->result().state != signal2m::UpdateState::checking; });
            const auto result = updater->result();
            require(result.state == signal2m::UpdateState::available && result.latestVersion == "9.9.9"
                    && result.pageUrl == fixture.url("/update-from-B1"),
                    "Overlapping native fetches must publish the updater's own version and B1 URL");
        }
        if (donations != nullptr)
        {
            waitUntil([&] { return donations->result().state != signal2m::DonationState::loading; });
            const auto result = donations->result();
            require(result.state == signal2m::DonationState::ready
                    && result.urls[0] == fixture.url(u8"/помощь?item=a,b")
                    && result.urls[1] == fixture.url("/donate-second"),
                    "Overlapping native fetches must preserve donation A1/A2 UTF-8 CSV independently of the update body");
        }
        updater.reset();
        donations.reset();
        // The diagnostic counters are per image, so quiescence is meaningful
        // only after BOTH owners have cancelled/completed and joined.
        requireQuiescent();
        require(fixture.accepted == 2 && fixture.privateHeaders == 0 && fixture.nonGetRequests == 0,
                "Concurrent feeds must make exactly two anonymous GETs without following either result URL");
        std::this_thread::sleep_for(20ms);
        requireQuiescent();
    }
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
        run("concurrent native update/donation results, independent cancellation and joined quiescence", concurrentUpdaterAndDonations);
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
