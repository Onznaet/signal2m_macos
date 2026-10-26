// Independent native loader: only C++/POSIX/libSystem, no JUCE or product code.
// It exercises the dynamic-image pin path and records actual post-dlclose
// residency separately. dlclose success alone is not proof of code unmapping.
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
unsigned assertions = 0;
unsigned normalReleases = 0, observedUnmapped = 0, observedResident = 0;
double maximumDestroyMs = 0.0;

void require(bool condition, const std::string& description)
{
    ++assertions;
    if (!condition) throw std::runtime_error(description);
}

template<class Predicate> void waitUntil(Predicate predicate, std::chrono::milliseconds budget = 1500ms)
{
    const auto end = Clock::now() + budget;
    while (!predicate())
    {
        require(Clock::now() < end, "native fixture did not reach the expected condition before its deadline");
        std::this_thread::sleep_for(2ms);
    }
}

bool sendAll(int socket, const std::string& bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const auto count = send(socket, bytes.data() + offset, bytes.size() - offset, 0);
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

enum class Peer { valid, headersStall, bodyStall, tlsStall };
class Fixture
{
public:
    explicit Fixture(Peer behavior) : mode(behavior)
    {
        listening = socket(AF_INET, SOCK_STREAM, 0);
        require(listening >= 0, "create native loopback socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listening, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 || listen(listening, 16) != 0)
        { close(listening); listening = -1; throw std::runtime_error("bind/listen on native loopback socket"); }
        socklen_t size = sizeof(address);
        if (getsockname(listening, reinterpret_cast<sockaddr*>(&address), &size) != 0)
        { close(listening); listening = -1; throw std::runtime_error("determine native fixture port"); }
        port = ntohs(address.sin_port);
        try { listener = std::thread([this] { listenLoop(); }); }
        catch (...) { close(listening); listening = -1; throw; }
    }
    ~Fixture()
    {
        stopped.store(true);
        if (listener.joinable()) listener.join();
        if (listening >= 0) close(listening);
        for (auto& client : clients) if (client.joinable()) client.join();
    }
    std::string endpoint() const
    {
        return std::string(mode == Peer::tlsStall ? "https://127.0.0.1:" : "http://127.0.0.1:")
            + std::to_string(port) + "/manifest";
    }
    std::atomic<unsigned> accepted{0}, requests{0}, bodyStarted{0}, clientHelloBytes{0}, landingRequests{0};
private:
    void listenLoop()
    {
        while (!stopped.load())
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listening, &readable);
            timeval interval{0, 20000};
            if (select(listening + 1, &readable, nullptr, nullptr, &interval) <= 0) continue;
            const auto client = accept(listening, nullptr, nullptr);
            if (client < 0) continue;
            ++accepted;
            clients.emplace_back([this, client]
            {
                timeval timeout{0, 250000};
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
                const int suppressSigPipe = 1;
                setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &suppressSigPipe, sizeof(suppressSigPipe));
                handle(client);
                shutdown(client, SHUT_RDWR);
                close(client);
            });
        }
    }
    void handle(int client)
    {
        std::string request;
        char bytes[2048];
        while (!stopped.load() && request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos)
        {
            const auto count = recv(client, bytes, sizeof(bytes), 0);
            if (count <= 0) return;
            if (mode == Peer::tlsStall)
            {
                clientHelloBytes += static_cast<unsigned>(count);
                while (!stopped.load()) std::this_thread::sleep_for(2ms);
                return;
            }
            request.append(bytes, static_cast<std::size_t>(count));
        }
        ++requests;
        if (request.rfind("GET /landing", 0) == 0) ++landingRequests;
        if (mode == Peer::headersStall)
        {
            while (!stopped.load()) std::this_thread::sleep_for(2ms);
            return;
        }
        const std::string body = "0.4,http://127.0.0.1:" + std::to_string(port) + "/landing\r\n";
        const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: text/csv\r\nContent-Length: "
            + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
        if (mode == Peer::bodyStall)
        {
            if (sendAll(client, header + body.substr(0, 1))) ++bodyStarted;
            while (!stopped.load()) std::this_thread::sleep_for(2ms);
            return;
        }
        sendAll(client, header + body);
    }
    const Peer mode;
    int listening = -1;
    unsigned short port = 0;
    std::atomic<bool> stopped{false};
    std::thread listener;
    std::vector<std::thread> clients;
};

struct Diagnostics
{
    std::uint64_t contexts = 0, callbacks = 0, fallbacks = 0, temporaryPins = 0, delegateClasses = 0;
    bool pinned = false;
};

class Library
{
public:
    using Make = void* (*)(const char*, std::uint32_t);
    using Start = bool (*)(void*);
    using State = unsigned (*)(void*);
    using Destroy = bool (*)(void*);
    using Read = void (*)(std::uint64_t*, std::uint64_t*, std::uint64_t*, std::uint64_t*, std::uint64_t*, bool*);
    using IsDynamic = bool (*)();
    using Force = void (*)();
    explicit Library(const std::filesystem::path& path) : name(path.u8string())
    {
        handle = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
        const auto error = dlerror();
        require(handle != nullptr, std::string("dlopen actual update fixture: ") + (error ? error : ""));
        try
        {
            make = symbol<Make>("ReviewerMacCreateChecker");
            start = symbol<Start>("ReviewerMacStartChecker");
            state = symbol<State>("ReviewerMacCheckerState");
            destroy = symbol<Destroy>("ReviewerMacDestroyChecker");
            read = symbol<Read>("ReviewerMacGetDiagnostics");
            isDynamic = symbol<IsDynamic>("ReviewerMacTransportIsDynamicImage");
            force = symbol<Force>("ReviewerMacForceNextFallback");
            require(isDynamic(), "transport code belongs to the loaded dylib, outside the main executable");
        }
        catch (...) { dlclose(handle); handle = nullptr; throw; }
    }
    ~Library() { if (handle) dlclose(handle); }
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    template<class T> T symbol(const char* wanted) const
    {
        auto* result = dlsym(handle, wanted);
        require(result != nullptr, std::string("dylib C export: ") + wanted);
        return reinterpret_cast<T>(result);
    }
    Diagnostics diagnostics() const
    {
        Diagnostics value;
        read(&value.contexts, &value.callbacks, &value.fallbacks, &value.temporaryPins, &value.delegateClasses, &value.pinned);
        return value;
    }
    void closeNormally()
    {
        closeHostReference();
        ++normalReleases;
        // Do not manufacture an unmapping claim. Objective-C/Foundation may
        // retain a library independently of our own balanced pin references.
        void* resident = dlopen(name.c_str(), RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
        if (resident) { ++observedResident; require(dlclose(resident) == 0, "release residency probe"); }
        else ++observedUnmapped;
        std::this_thread::sleep_for(20ms); // Let any erroneous late callback surface.
    }
    void closeAfterFallback()
    {
        closeHostReference();
        void* resident = dlopen(name.c_str(), RTLD_NOLOAD | RTLD_LAZY | RTLD_LOCAL);
        require(resident != nullptr, "permanent fallback pin protects the image after the host releases it");
        auto diagnostic = reinterpret_cast<Read>(dlsym(resident, "ReviewerMacGetDiagnostics"));
        require(diagnostic != nullptr, "retained fixture remains callable through a fresh loader reference");
        Diagnostics value;
        diagnostic(&value.contexts, &value.callbacks, &value.fallbacks, &value.temporaryPins, &value.delegateClasses, &value.pinned);
        require(value.pinned && value.fallbacks == 1 && value.temporaryPins == 0 && value.delegateClasses == 1,
                "one permanent pin and no temporary references survive fallback host release");
        require(dlclose(resident) == 0, "release fallback residency probe; permanent pin is never reset");
    }
    Make make = nullptr;
    Start start = nullptr;
    State state = nullptr;
    Destroy destroy = nullptr;
    Read read = nullptr;
    IsDynamic isDynamic = nullptr;
    Force force = nullptr;
private:
    void closeHostReference()
    {
        require(handle != nullptr, "host loader reference exists");
        const auto result = dlclose(handle);
        handle = nullptr;
        require(result == 0, "dlclose host reference after checker destruction");
    }
    std::string name;
    void* handle = nullptr;
};

class Checker
{
public:
    Checker(Library& owner, const std::string& endpoint, std::uint32_t milliseconds = 1500) : library(owner)
    {
        value = library.make(endpoint.c_str(), milliseconds);
        require(value != nullptr, "create checker through actual dylib C boundary");
        require(library.state(value) == 0, "construction creates no native request");
    }
    ~Checker() { if (value) library.destroy(value); }
    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;
    void start() { require(library.start(value), "explicit update starts in the dynamic image"); }
    unsigned terminal()
    {
        waitUntil([this] { return library.state(value) != 1; }, 2500ms);
        const auto result = library.state(value);
        require(result == 2 || result == 3 || result == 4, "checker reaches an actual terminal state");
        return result;
    }
    void destroy()
    {
        const auto before = Clock::now();
        const bool destroyed = library.destroy(value);
        value = nullptr;
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - before).count();
        maximumDestroyMs = std::max(maximumDestroyMs, elapsed);
        require(destroyed && elapsed < 700.0, "checker cancellation/join/native cleanup stays below 700ms");
    }
private:
    Library& library;
    void* value = nullptr;
};

void quiescent(const Library& library)
{
    const auto value = library.diagnostics();
    require(value.contexts == 0 && value.callbacks == 0 && value.temporaryPins == 0 && value.delegateClasses == 0,
            "normal dynamic fetch has no surviving heap context, callback, delegate class, or temporary image pin");
    require(value.fallbacks == 0 && !value.pinned, "normal dynamic fetch does not permanently pin its image");
}

void successfulReleases(const std::filesystem::path& fixture)
{
    Fixture peer(Peer::valid);
    for (unsigned iteration = 0; iteration < 8; ++iteration)
    {
        Library library(fixture);
        quiescent(library);
        Checker checker(library, peer.endpoint());
        const auto before = peer.requests.load();
        std::this_thread::sleep_for(10ms);
        require(peer.requests == before, "no network during dylib checker construction/polling");
        checker.start();
        require(checker.terminal() == 3, "real dynamic-image HTTP fetch publishes the newer CSV version");
        checker.destroy();
        quiescent(library);
        library.closeNormally();
    }
    require(peer.requests == 8 && peer.landingRequests == 0, "one manifest GET per request; B1 is never fetched automatically");
    std::cout << "PASS dynamic-image success, repeated loader release, no B1 fetch, balanced image pins\n";
}

void cancelAndDeadline(const std::filesystem::path& fixture, Peer mode)
{
    Fixture peer(mode);
    for (unsigned iteration = 0; iteration < 6; ++iteration)
    {
        Library library(fixture);
        Checker checker(library, peer.endpoint());
        const auto requests = peer.requests.load();
        const auto bytes = peer.clientHelloBytes.load();
        const auto bodies = peer.bodyStarted.load();
        checker.start();
        waitUntil([&]
        {
            if (mode == Peer::tlsStall) return peer.clientHelloBytes > bytes;
            if (mode == Peer::bodyStall) return peer.bodyStarted > bodies;
            return peer.requests > requests;
        });
        checker.destroy();
        quiescent(library);
        library.closeNormally();
    }
    Library library(fixture);
    Checker timeout(library, peer.endpoint(), 150);
    const auto before = Clock::now();
    timeout.start();
    require(timeout.terminal() == 4, "stalled native header/body/TLS reaches failed deadline");
    const auto duration = Clock::now() - before;
    require(duration >= 100ms && duration < 800ms, "absolute deadline includes native connect/read/TLS and cleanup grace");
    timeout.destroy();
    quiescent(library);
    library.closeNormally();
    std::cout << "PASS dynamic-image cancel/deadline/loader release mode=" << static_cast<unsigned>(mode) << '\n';
}

void forcedFallbackLast(const std::filesystem::path& fixture)
{
    Fixture peer(Peer::valid);
    Library library(fixture);
    quiescent(library);
    library.force(); // Fixture-only seam; never compiled into the product target.
    Checker checker(library, peer.endpoint());
    checker.start();
    require(checker.terminal() == 4, "forced native teardown fallback fails closed");
    checker.destroy();
    const auto initial = library.diagnostics();
    require(initial.fallbacks == 1 && initial.pinned && initial.contexts == 1
            && initial.callbacks == 0 && initial.temporaryPins == 0 && initial.delegateClasses == 1,
            "one fallback quarantines exactly one context/class and one permanent image reference");
    const auto requests = peer.requests.load();
    for (unsigned iteration = 0; iteration < 8; ++iteration)
    {
        Checker retry(library, peer.endpoint());
        retry.start();
        require(retry.terminal() == 4, "post-fallback check is rejected by the per-image circuit breaker");
        retry.destroy();
        const auto value = library.diagnostics();
        require(value.contexts == initial.contexts && value.callbacks == 0 && value.fallbacks == 1
                && value.temporaryPins == 0 && value.delegateClasses == 1 && value.pinned,
                "repeated requests cannot accumulate quarantined contexts or loader references");
    }
    require(peer.requests == requests, "post-fallback checks create no additional native requests");
    library.closeAfterFallback();
    std::cout << "PASS real permanent-pin fallback, bounded per-image retention, fail-closed circuit and host dlclose\n";
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    if (argc != 3 || std::strcmp(argv[1], "--fixture") != 0)
    { std::cerr << "Usage: Signal2MReviewerMacUpdateLifecycle --fixture <actual-test-dylib>\n"; return 2; }
    try
    {
        const auto fixture = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
        require(std::filesystem::is_regular_file(fixture), "actual update dylib fixture exists");
        successfulReleases(fixture);
        for (const auto mode : {Peer::headersStall, Peer::bodyStall, Peer::tlsStall}) cancelAndDeadline(fixture, mode);
        // This must be last: neither production nor tests expose an unsafe
        // reset/unpin operation for a quarantined native callback image.
        forcedFallbackLast(fixture);
    }
    catch (const std::exception& error)
    { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
    std::cout << "METRIC normal_host_dlclose=" << normalReleases << " observed_unmapped=" << observedUnmapped
              << " observed_resident=" << observedResident << " max_destroy_ms=" << maximumDestroyMs << '\n';
    std::cout << "RESULT native_dynamic_lifetime_assertions=" << assertions << " failures=0\n";
    return 0;
}
