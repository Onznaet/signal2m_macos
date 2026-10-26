#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
#else
 #include <arpa/inet.h>
 #include <netinet/in.h>
 #include <sys/select.h>
 #include <sys/socket.h>
 #include <sys/time.h>
 #include <unistd.h>
 using SOCKET = int;
 constexpr SOCKET INVALID_SOCKET = -1;
 constexpr int SD_BOTH = SHUT_RDWR;
 inline int closesocket(SOCKET handle) { return ::close(handle); }
#endif
#include "UpdateChecker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using signal2m::UpdateChecker;
using signal2m::UpdateState;
namespace detail = signal2m::update_detail;
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2000ms)
{
    const auto end = Clock::now() + timeout;
    while (!predicate())
    {
        if (Clock::now() >= end) throw std::runtime_error("Timed out waiting for a test condition");
        std::this_thread::sleep_for(2ms);
    }
}

signal2m::UpdateResult terminal(UpdateChecker& checker, std::chrono::milliseconds timeout = 2000ms)
{
    waitUntil([&] { return checker.result().state != UpdateState::checking; }, timeout);
    return checker.result();
}

detail::FetchResponse validResponse()
{
    return { true, 200, "text/csv;charset=utf-8", "0.4,https://vk.ru/lmzvuk\r\n" };
}

void testConfiguredFeed()
{
    const auto endpoint = detail::defaultEndpoint();
    require(endpoint.startsWith("https://docs.google.com/spreadsheets/d/"),
            "Production feed must use the public Google spreadsheet over HTTPS");
    require(endpoint.contains("&range=A1%3AB1"), "Production feed must read only A1:B1");
   #if defined(SIGNAL2M_UPDATE_FEED_URL)
    require(endpoint == SIGNAL2M_UPDATE_FEED_URL, "Checker must use the configured build feed");
   #elif JUCE_MAC
    require(endpoint.contains("gid=640439888&"), "The default Mac feed must use its dedicated sheet");
   #else
    require(endpoint.contains("gid=0&"), "The Windows feed must remain on its existing sheet");
   #endif
}

void testVersions()
{
    require(detail::compareVersions("0.1", "0.1.0") == 0, "Missing trailing version parts must be zero");
    require(detail::compareVersions("0.10", "0.9") == 1, "Versions must be compared numerically, not lexically");
    require(detail::compareVersions("00.004.000", "0.4") == 0, "Leading zeros must be supported");
    require(detail::compareVersions("1.0.0.1", "1") == 1, "All numeric version components must participate");
    require(detail::compareVersions("0.3.0", "0.4") == -1, "The current build must detect the sheet's 0.4 release");
    require(detail::compareVersions("0.3.1", "0.4") == -1, "Version 0.3.1 must still detect the sheet's 0.4 release");
    require(detail::compareVersions("4294967295", "4294967294") == 1, "Largest accepted numeric component must compare safely");
    for (const char* version : { "", ".1", "1.", "1..0", "1.0.", "v0.4", "0.4-beta", "0.4+1", "-1", "+1",
                                 "1e2", " 0.4", "0.4 ", "4294967296", "9999999999999999999999999999" })
        require(!detail::compareVersions(version, "0.3.0"), "Malformed/suffixed/overflowing versions must fail");
}

void testManifestAndUrls()
{
    const auto manifest = detail::parseManifest("\xef\xbb\xbf\"00.4\",\"https://example.org/news%20page?q=hello%20world\"\r\n"
                                               "99,https://other.invalid/ignored\r\n");
    require(manifest.has_value(), "UTF-8 BOM, quoted CSV, CRLF and encoded spaces must parse");
    require(manifest->version == "00.4" && manifest->pageUrl == "https://example.org/news%20page?q=hello%20world",
            "Only A1 and B1 may define the release result");
    require(detail::parseManifest("0.4,\"https://example.org/?a=1,b=2\",\"ignored \"\"quoted\"\" field\"\n").has_value(),
            "Quoted commas and escaped quotes must be recognized without changing B1");
    require(detail::parseManifest(" 0.4 , https://vk.ru/lmzvuk \n").has_value(), "Surrounding cell whitespace may be trimmed");
    require(detail::parseManifest("0.4,https://example.org/\n\"ignored\nmultiline\",also-ignored\n").has_value(),
            "Quoted multiline ignored rows must not replace A1/B1");
    for (const char* body : { "", "Version,URL", "0.4", "0.4,", "0.4,file:///tmp/plugin", "0.4,\"https://example.org/",
                              "0.4,https://example.org/\"", "\"0.4\"bad,https://example.org/", "<html>0.4</html>",
                              "0.4,https://example.org/\r", "0.4,https://example.org/\n\"unterminated" })
        require(!detail::parseManifest(body), "Invalid CSV or release metadata must fail");
    require(!detail::parseManifest(std::string("0.4,https://example.org/\0suffix", 32)), "Embedded NUL must not truncate URL validation");
    for (const auto& invalid : { std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
                                 std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2) })
        require(!detail::parseManifest("0.4,https://example.org/" + invalid), "UTF-8 must reject overlong, surrogate, out-of-range and truncated sequences");
    require(!detail::parseManifest(std::string(detail::maximumBodyBytes + 1, 'a')), "Parser must independently enforce the body cap");
    for (const char* url : { "https://vk.ru/lmzvuk", "HTTP://example.org:8080/a?b=c#d", "https://localhost/",
                             "http://127.0.0.1:1234/", "https://[::1]/", "https://[2001:db8::1]/", "https://[::]/",
                             "https://[::ffff:192.0.2.1]/", "https://example.org/a%20b" })
        require(detail::isSafeHttpUrl(url), "Absolute HTTP(S) URLs must be accepted");
    for (const char* url : { "javascript:alert(1)", "file:///etc/passwd", "data:text/html,test", "ftp://example.org/", "//example.org/",
                             "/relative", "https:///empty", "https://?query", "https://", "https://user:secret@example.org/",
                             "https://example.org\\@evil.org/", "https://example.org/a b", "https://example.org/%0d%0aInjected",
                             "https://example.org/%00", "https://example.org/%7f", "https://example.org/%zz", "https://example.org:abc/",
                             "https://example.org:65536/", "https://-bad.example/", "https://a..example/", "https://./",
                             "https://[:::]/", "https://[1:2:3]/", "https://[1::2::3]/", "https://[::ffff:999.0.0.1]/" })
        require(!detail::isSafeHttpUrl(url), "Unsafe, relative or hostless URLs must fail");
}

void testExplicitStartDuplicateAndCache()
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    detail::CheckerOptions options;
    options.fetch = [calls](const juce::String& endpoint, detail::RequestControl& control)
    {
        require(endpoint == detail::defaultEndpoint(), "Production source must stay pinned");
        ++*calls;
        control.waitFor(100ms);
        return validResponse();
    };
    UpdateChecker checker("0.3.0", options);
    require(checker.installedVersion() == "0.3.0" && checker.result().state == UpdateState::idle, "Construction must expose idle installed state");
    std::this_thread::sleep_for(20ms);
    require(calls->load() == 0, "Construction and result polling must never fetch automatically");
    require(checker.checkForUpdates(), "Explicit check must start a worker");
    waitUntil([&] { return calls->load() == 1; });
    for (int i = 0; i < 20; ++i)
        require(!checker.checkForUpdates(), "Repeated clicks must not create duplicate in-flight requests");
    const auto result = terminal(checker);
    require(result.state == UpdateState::available && result.latestVersion == "0.4" && result.pageUrl == "https://vk.ru/lmzvuk",
            "Explicit response must publish version and dynamic B1 URL together");
    std::this_thread::sleep_for(30ms);
    require(calls->load() == 1, "A cached result must not schedule another request");
    require(checker.checkForUpdates(), "A completed checker must allow a new explicit retry");
    require(terminal(checker).state == UpdateState::available && calls->load() == 2, "Explicit retry must fetch again");
}

void testErrorsRetryAndComparisonStates()
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    detail::CheckerOptions options;
    options.fetch = [calls](const juce::String&, detail::RequestControl&)
    {
        auto response = validResponse();
        if (++*calls == 1) response.statusCode = 503;
        return response;
    };
    UpdateChecker checker("0.3.0", options);
    require(checker.checkForUpdates() && terminal(checker).state == UpdateState::failed, "HTTP failure must be reported");
    require(checker.checkForUpdates() && terminal(checker).state == UpdateState::available, "A failed request must permit explicit retry");
    for (const char* installed : { "0.4.0", "0.5", "1.0" })
    {
        UpdateChecker current(installed, options);
        require(current.checkForUpdates() && terminal(current).state == UpdateState::upToDate, "Equal or older remote versions must not advertise an update");
    }
    for (int mode = 0; mode < 4; ++mode)
    {
        options.fetch = [mode](const juce::String&, detail::RequestControl&)
        {
            auto response = validResponse();
            if (mode == 0) response.success = false;
            if (mode == 1) response.contentType = "text/html";
            if (mode == 2) response.body = "<html>login</html>";
            if (mode == 3) throw std::runtime_error("Injected transport exception");
            return response;
        };
        UpdateChecker failed("0.3.0", options);
        require(failed.checkForUpdates() && terminal(failed).state == UpdateState::failed, "Network, HTML, data and exception failures must become failed state");
    }
}

void testStartupOnceManualRetryAndNewInstances()
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto release = std::make_shared<std::atomic<bool>>(false);
    detail::CheckerOptions options;
    options.fetch = [calls, release](const juce::String&, detail::RequestControl& control)
    {
        ++*calls;
        while (!release->load() && !control.stopRequested()) control.waitFor(2ms);
        return validResponse();
    };
    UpdateChecker first("0.3.1", options);
    std::this_thread::sleep_for(15ms);
    require(first.installedVersion() == "0.3.1" && first.result().state == UpdateState::idle && calls->load() == 0,
            "Constructing a processor-owned checker must remain idle until an editor startup hook");
    require(first.checkOnStartup(), "The first visible editor startup must initiate a check");
    require(first.result().state == UpdateState::checking, "The startup hook must synchronously publish checking state");
    for (int i = 0; i < 20; ++i)
        require(!first.checkOnStartup(), "Repeated startup callbacks must not launch duplicate work");
    release->store(true);
    require(terminal(first).state == UpdateState::available && calls->load() == 1,
            "One startup attempt must publish the ordinary version/link result");
    for (int i = 0; i < 20; ++i)
        require(!first.checkOnStartup(), "Closing and reopening an editor must not repeat a completed startup request");
    require(first.checkForUpdates() && terminal(first).state == UpdateState::available && calls->load() == 2,
            "A completed startup check must leave the manual retry path available");
    require(!first.checkOnStartup(), "A manual retry must never rearm automatic startup");
    require(first.checkForUpdates() && terminal(first).state == UpdateState::available && calls->load() == 3,
            "Manual checking must remain repeatable after multiple attempts");

    UpdateChecker second("0.3.1", options);
    require(second.result().state == UpdateState::idle && calls->load() == 3,
            "A new processor checker must not inherit another instance's result or perform constructor I/O");
    require(second.checkOnStartup() && terminal(second).state == UpdateState::available && calls->load() == 4,
            "A different processor instance must have its own startup opportunity");
    require(!first.checkOnStartup() && !second.checkOnStartup(), "Startup consumption must persist independently per instance");
}

void testStartupBusyFailureAndCancellation()
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto release = std::make_shared<std::atomic<bool>>(false);
    detail::CheckerOptions options;
    options.fetch = [calls, release](const juce::String&, detail::RequestControl& control)
    {
        ++*calls;
        while (!release->load() && !control.stopRequested()) control.waitFor(2ms);
        return validResponse();
    };
    UpdateChecker busy("0.3.1", options);
    require(busy.checkForUpdates(), "A manual request may already be running before the startup callback");
    waitUntil([&] { return calls->load() == 1; });
    require(!busy.checkOnStartup(), "Startup must share an existing manual request rather than run in parallel");
    release->store(true);
    require(terminal(busy).state == UpdateState::available && calls->load() == 1, "The original manual request must finish normally");
    require(!busy.checkOnStartup(), "A busy startup attempt must still be permanently consumed");
    require(busy.checkForUpdates() && terminal(busy).state == UpdateState::available && calls->load() == 2,
            "A busy startup attempt must not disable future manual checks");

    auto failureCalls = std::make_shared<std::atomic<int>>(0);
    options.fetch = [failureCalls](const juce::String&, detail::RequestControl& control)
    {
        control.waitFor(40ms);
        auto response = validResponse();
        if (++*failureCalls == 1) response.statusCode = 503;
        return response;
    };
    UpdateChecker failed("0.3.1", options);
    require(failed.checkOnStartup() && terminal(failed).state == UpdateState::failed,
            "A failed automatic check must publish the ordinary failure state");
    for (int i = 0; i < 20; ++i)
        require(!failed.checkOnStartup(), "Failed startup checks must not retry through later editor callbacks");
    require(failed.checkForUpdates() && terminal(failed).state == UpdateState::available && failureCalls->load() == 2,
            "After startup failure, a deliberate manual retry may succeed");
    require(!failed.checkOnStartup(), "Successful manual recovery must not reset the once flag");

    auto entered = std::make_shared<std::atomic<int>>(0);
    options.fetch = [entered](const juce::String&, detail::RequestControl& control)
    {
        ++*entered;
        control.waitFor(1000ms);
        return validResponse();
    };
    UpdateChecker cancelled("0.3.1", options);
    require(cancelled.checkOnStartup(), "A cancellable startup request must start once");
    waitUntil([&] { return entered->load() == 1; });
    cancelled.cancel();
    require(terminal(cancelled).state == UpdateState::idle && !cancelled.checkOnStartup(),
            "Cancellation must consume the original startup opportunity too");
}

void testConcurrentStartupCallbacks()
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto release = std::make_shared<std::atomic<bool>>(false);
    detail::CheckerOptions options;
    options.fetch = [calls, release](const juce::String&, detail::RequestControl& control)
    {
        ++*calls;
        while (!release->load() && !control.stopRequested()) control.waitFor(2ms);
        return validResponse();
    };
    UpdateChecker checker("0.3.1", options);
    std::atomic<int> started { 0 }, sawChecking { 0 };
    std::vector<std::thread> callbacks;
    for (int i = 0; i < 16; ++i)
        callbacks.emplace_back([&]
        {
            if (checker.checkOnStartup())
            {
                ++started;
                if (checker.result().state == UpdateState::checking) ++sawChecking;
            }
        });
    for (auto& callback : callbacks) callback.join();
    waitUntil([&] { return calls->load() == 1; });
    require(started.load() == 1 && sawChecking.load() == 1,
            "Concurrent repeated startup callbacks must have one winner with synchronous checking state");
    release->store(true);
    require(terminal(checker).state == UpdateState::available && calls->load() == 1,
            "Concurrent startup must still use exactly one shared request");
}

void testDeadlineCancellationAndDestruction()
{
    auto entered = std::make_shared<std::atomic<int>>(0);
    detail::CheckerOptions options;
    options.timeout = 150ms;
    options.fetch = [entered](const juce::String&, detail::RequestControl& control)
    {
        ++*entered;
        control.waitFor(10000ms); // Same cancellation contract during an injected DNS/connection stage.
        --*entered;
        return validResponse();
    };
    {
        UpdateChecker timeout("0.3.0", options);
        const auto start = Clock::now();
        require(timeout.checkForUpdates(), "Deadline test must start explicitly");
        require(terminal(timeout).state == UpdateState::failed, "An expired total deadline must reject even a late valid response");
        require(Clock::now() - start >= 120ms && Clock::now() - start < 700ms, "Deadline must bound the whole injected transport stage");
    }
    options.timeout = 8000ms;
    {
        UpdateChecker cancelled("0.3.0", options);
        require(cancelled.checkForUpdates(), "Cancel test must start explicitly");
        waitUntil([&] { return entered->load() != 0; });
        const auto start = Clock::now();
        cancelled.cancel();
        require(terminal(cancelled).state == UpdateState::idle, "Explicit cancellation returns to idle");
        require(Clock::now() - start < 500ms, "Cancellation must wake pending work promptly");
    }
    const auto start = Clock::now();
    {
        UpdateChecker destroyed("0.3.0", options);
        require(destroyed.checkForUpdates(), "Destruction test must start explicitly");
        waitUntil([&] { return entered->load() != 0; });
    }
    require(entered->load() == 0 && Clock::now() - start < 500ms, "Destructor must cancel and join its worker without detaching it");
}

bool sendAll(SOCKET socket, const std::string& data)
{
    std::size_t offset = 0;
    while (offset < data.size())
    {
        const int count = send(socket, data.data() + offset, static_cast<int>(data.size() - offset), 0);
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool pauseFor(std::chrono::milliseconds duration, const std::atomic<bool>& stop)
{
    const auto end = Clock::now() + duration;
    while (Clock::now() < end && !stop.load()) std::this_thread::sleep_for(5ms);
    return !stop.load();
}

class LoopbackServer
{
public:
    using Handler = std::function<void(SOCKET, const std::string&, const std::atomic<bool>&)>;
    explicit LoopbackServer(Handler callback) : handler(std::move(callback))
    {
        listening = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listening != INVALID_SOCKET, "Loopback socket creation failed");
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listening, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Loopback bind failed");
       #if defined(_WIN32)
        int size = sizeof(address);
       #else
        socklen_t size = sizeof(address);
       #endif
        require(getsockname(listening, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Loopback port query failed");
        port = ntohs(address.sin_port);
        require(listen(listening, 16) == 0, "Loopback listen failed");
        worker = std::thread([this]
        {
            while (!stopped.load())
            {
                fd_set readers;
                FD_ZERO(&readers);
                FD_SET(listening, &readers);
                timeval timeout { 0, 20000 };
               #if defined(_WIN32)
                const int descriptorLimit = 0; // WinSock ignores this argument.
               #else
                const int descriptorLimit = listening + 1;
               #endif
                if (select(descriptorLimit, &readers, nullptr, nullptr, &timeout) <= 0) continue;
                SOCKET client = accept(listening, nullptr, nullptr);
                if (client == INVALID_SOCKET) continue;
                ++accepted;
                clients.emplace_back([this, client]
                {
                   #if defined(_WIN32)
                    const DWORD socketTimeout = 300;
                   #else
                    const timeval socketTimeout { 0, 300000 };
                   #if defined(SO_NOSIGPIPE)
                    const int noSigPipe = 1;
                    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
                   #endif
                   #endif
                    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&socketTimeout), sizeof(socketTimeout));
                    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&socketTimeout), sizeof(socketTimeout));
                    std::string request;
                    char buffer[1024];
                    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192)
                    {
                        const int count = recv(client, buffer, sizeof(buffer), 0);
                        if (count <= 0) break;
                        request.append(buffer, static_cast<std::size_t>(count));
                    }
                    const auto first = request.find(' '), last = request.find(' ', first + 1);
                    if (first != std::string::npos && last != std::string::npos)
                        handler(client, request.substr(first + 1, last - first - 1), stopped);
                    shutdown(client, SD_BOTH);
                    closesocket(client);
                });
            }
        });
    }
    ~LoopbackServer()
    {
        stopped.store(true);
        worker.join();
        closesocket(listening);
        for (auto& client : clients) client.join();
    }
    juce::String url(const char* path) const { return "http://127.0.0.1:" + juce::String(port) + path; }
    std::atomic<int> accepted { 0 };
private:
    SOCKET listening = INVALID_SOCKET;
    unsigned short port = 0;
    Handler handler;
    std::atomic<bool> stopped { false };
    std::thread worker;
    std::vector<std::thread> clients;
};

void response(SOCKET socket, const std::string& body, int status = 200, const char* type = "text/csv;charset=utf-8")
{
    sendAll(socket, "HTTP/1.1 " + std::to_string(status) + " Test\r\nContent-Type: " + type
                    + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
}

void testNativeHttpDataRedirectsAndBounds()
{
    std::atomic<int> pageRequests { 0 };
    juce::String page;
    LoopbackServer server([&](SOCKET client, const std::string& path, const std::atomic<bool>&)
    {
        if (path == "/page") { ++pageRequests; response(client, "not-a-manifest"); return; }
        if (path.rfind("/r/", 0) == 0)
        {
            const int remaining = std::stoi(path.substr(3));
            if (remaining > 0)
            {
                sendAll(client, "HTTP/1.1 302 Redirect\r\nLocation: /r/" + std::to_string(remaining - 1)
                                + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                return;
            }
        }
        if (path == "/oversize") { response(client, std::string(detail::maximumBodyBytes + 1, 'a')); return; }
        if (path == "/chunked-overflow")
        {
            if (!sendAll(client, "HTTP/1.1 200 OK\r\nContent-Type: text/csv\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")) return;
            for (int i = 0; i < 9; ++i)
                if (!sendAll(client, "2000\r\n" + std::string(8192, 'a') + "\r\n")) return;
            sendAll(client, "0\r\n\r\n");
            return;
        }
        const std::string body = "0.4," + page.toStdString();
        if (path == "/404") { response(client, body, 404); return; }
        if (path == "/html") { response(client, body, 200, "text/html"); return; }
        response(client, body);
    });
    page = server.url("/page");
    for (const char* path : { "/ok", "/r/3" })
    {
        detail::CheckerOptions options;
        options.endpoint = server.url(path);
        UpdateChecker checker("0.3.1", options);
        require(checker.checkForUpdates(), "Native HTTP request must start");
        const auto result = terminal(checker);
        require(result.state == UpdateState::available && result.pageUrl == page, "Native CSV/redirect response must publish its actual B1 URL");
    }
    require(pageRequests.load() == 0, "B1 URL must never be fetched by UpdateChecker");
    for (const char* path : { "/404", "/html", "/r/4", "/oversize", "/chunked-overflow" })
    {
        detail::CheckerOptions options;
        options.endpoint = server.url(path);
        UpdateChecker checker("0.3.0", options);
        require(checker.checkForUpdates() && terminal(checker).state == UpdateState::failed,
                "Native HTTP errors, HTML, excessive redirects and oversized bodies must fail");
    }
    detail::RequestControl control(Clock::now() + 2s);
    const auto oversized = detail::fetchHttp(server.url("/chunked-overflow"), control);
    require(!oversized.success && oversized.body.size() <= detail::maximumBodyBytes,
            "Actual transport must enforce its cap without trusting Content-Length");
}

void testNativeGlobalDeadlineCancelAndUnload()
{
    LoopbackServer server([](SOCKET client, const std::string& path, const std::atomic<bool>& stop)
    {
        if (path == "/header-stall") { pauseFor(5000ms, stop); return; }
        const std::string body = "0.4,https://vk.ru/lmzvuk";
        if (!sendAll(client, "HTTP/1.1 200 OK\r\nContent-Type: text/csv\r\nContent-Length: "
                             + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n")) return;
        for (char character : body)
        {
            if (!pauseFor(25ms, stop) || !sendAll(client, std::string(1, character))) break;
        }
    });
    for (const char* path : { "/header-stall", "/slow-body" })
    {
        detail::CheckerOptions options;
        options.endpoint = server.url(path);
        options.timeout = 150ms;
        UpdateChecker checker("0.3.0", options);
        const auto start = Clock::now();
        require(checker.checkForUpdates() && terminal(checker).state == UpdateState::failed, "Header/body stall must reach a failed deadline");
        const auto elapsed = Clock::now() - start;
        require(elapsed >= 120ms && elapsed < 700ms, "One deadline must include slow-body reads and native callback drain");
    }
    for (const char* path : { "/header-stall", "/slow-body" })
    {
        detail::CheckerOptions options;
        options.endpoint = server.url(path);
        const auto requests = server.accepted.load();
        auto checker = std::make_unique<UpdateChecker>("0.3.0", options);
        require(checker->checkForUpdates(), "Unload test must start a native request");
        waitUntil([&] { return server.accepted.load() > requests; });
        std::this_thread::sleep_for(15ms);
        const auto start = Clock::now();
        checker.reset();
        require(Clock::now() - start < 700ms, "Unload must cancel native connect/header/body and drain every callback promptly");
    }
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
   #if defined(_WIN32)
    WSADATA data {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 2;
   #endif
    if (argc == 2 && std::string(argv[1]) == "--live")
    {
        detail::FetchResponse diagnostic;
        detail::CheckerOptions options;
        options.fetch = [&](const juce::String& endpoint, detail::RequestControl& control)
        {
            diagnostic = detail::fetchHttp(endpoint, control);
            return diagnostic;
        };
        UpdateChecker checker("0.3.1", options);
        checker.checkForUpdates();
        const auto result = terminal(checker, 10000ms);
        std::cout << "Live state=" << static_cast<int>(result.state) << " latest=" << result.latestVersion
                  << " page=" << result.pageUrl << '\n';
        std::cout << "Native stage=" << diagnostic.stage << " error=" << diagnostic.nativeError
                  << " secureFlags=" << diagnostic.secureFailureFlags << " status=" << diagnostic.statusCode
                  << " type=" << diagnostic.contentType << " bytes=" << diagnostic.body.size() << '\n';
       #if defined(_WIN32)
        WSACleanup();
       #endif
        return result.state == UpdateState::available || result.state == UpdateState::upToDate ? 0 : 1;
    }
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        { "platform-specific production feed binding", testConfiguredFeed },
        { "numeric versions and malformed/overflow rejection", testVersions },
        { "strict UTF-8 CSV A1/B1 and safe dynamic URLs", testManifestAndUrls },
        { "explicit start, no automatic request, duplicate suppression and cache", testExplicitStartDuplicateAndCache },
        { "errors, explicit retry and version result states", testErrorsRetryAndComparisonStates },
        { "startup once, manual retries and independent new instances", testStartupOnceManualRetryAndNewInstances },
        { "startup while busy, failure recovery and cancellation", testStartupBusyFailureAndCancellation },
        { "concurrent startup callbacks share one request", testConcurrentStartupCallbacks },
        { "total deadline, cancellation and joined lifetime", testDeadlineCancellationAndDestruction },
        { "native loopback HTTP, redirects, no B1 fetch and body bounds", testNativeHttpDataRedirectsAndBounds },
        { "native header/slow-body deadline, cancel and unload drain", testNativeGlobalDeadlineCancelAndUnload }
    };
    int failed = 0;
    for (const auto& test : tests)
    {
        try
        {
           #if JUCE_MAC
            // The production transport accepts background callers only. Keep
            // direct transport probes off the main thread just like the worker.
            std::exception_ptr failure;
            std::thread runner([&]
            {
                try { test.run(); }
                catch (...) { failure = std::current_exception(); }
            });
            runner.join();
            if (failure) std::rethrow_exception(failure);
           #else
            test.run();
           #endif
            std::cout << "PASS " << test.name << '\n';
        }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << test.name << ": " << error.what() << '\n'; }
    }
   #if defined(_WIN32)
    WSACleanup();
   #endif
    std::cout << (failed == 0 ? "All UpdateChecker checks passed.\n" : "UpdateChecker checks failed.\n");
    return failed == 0 ? 0 : 1;
}
