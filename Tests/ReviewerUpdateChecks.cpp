// Independent expectations for manual update parsing and the real HTTP transport.
// No editor, browser, installation, or spreadsheet writes.
#include "UpdateChecker.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using signal2m::UpdateChecker;
using signal2m::UpdateResult;
using signal2m::UpdateState;
namespace detail = signal2m::update_detail;
namespace
{
int assertions = 0, failures = 0;
using Clock = std::chrono::steady_clock;

void expect (bool condition, const std::string& message)
{
    ++assertions;
    if (! condition)
    {
        ++failures;
        std::cout << "FAIL " << message << '\n';
    }
}

double elapsedMs (Clock::time_point start)
{
    return std::chrono::duration<double, std::milli> (Clock::now() - start).count();
}

UpdateResult waitDone (UpdateChecker& checker, std::chrono::milliseconds maximum = 10500ms)
{
    const auto end = Clock::now() + maximum;
    for (;;)
    {
        auto result = checker.result();
        if (result.state != UpdateState::checking)
            return result;
        if (Clock::now() >= end)
        {
            checker.cancel();
            throw std::runtime_error ("update check failed to reach a terminal state within test deadline");
        }
        std::this_thread::sleep_for (2ms);
    }
}

detail::CheckerOptions at (const std::string& endpoint, std::chrono::milliseconds timeout = 8000ms)
{
    detail::CheckerOptions options;
    options.endpoint = juce::String::fromUTF8 (endpoint.c_str());
    options.timeout = timeout;
    return options;
}

int count (const std::string& base, const std::string& path)
{
    const juce::URL endpoint (juce::String (base + "/__count?path=")
                             + juce::URL::addEscapeChars (juce::String (path), true));
    auto stream = endpoint.createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                               .withConnectionTimeoutMs (1500).withNumRedirectsToFollow (0));
    if (stream == nullptr)
        throw std::runtime_error ("controlled HTTP server did not answer count query");
    return stream->readEntireStreamAsString().getIntValue();
}

void parserChecks()
{
    struct Pair { const char* a; const char* b; int expected; };
    for (const auto pair : std::vector<Pair> {{"0.1", "0.1.0", 0}, {"0.3.0", "0.3", 0},
             {"0.10", "0.9", 1}, {"0.3.0.1", "0.3", 1}, {"1.0", "0.9999.99", 1},
             {"12.2", "12.10", -1}, {"000.003.0", "0.3", 0},
             {"4294967295.0", "4294967294.99", 1}})
    {
        const auto forward = detail::compareVersions (pair.a, pair.b);
        const auto reverse = detail::compareVersions (pair.b, pair.a);
        expect (forward && *forward == pair.expected, std::string ("numeric version ") + pair.a + " vs " + pair.b);
        expect (reverse && *reverse == -pair.expected, "numeric comparison is symmetric under reversal");
    }
    for (const std::string bad : {"", ".", "0.", ".4", "0..4", "-1.0", "+1.0", "0.3-beta",
             "0.3+build", "v0.4", "0.4 ", "0. 4", "0,4", "4294967296", "9999999999999999999999999"})
    {
        expect (! detail::compareVersions (bad, "0.3.0"), "malformed/overflow latest version rejected: " + bad);
        expect (! detail::compareVersions ("0.3.0", bad), "malformed installed version is an error");
    }

    for (const auto good : {"https://example.test/releases", "http://127.0.0.1:8080/p?q=a,b", "HTTPS://example.test/x",
             "http://[::1]:8080/p", "https://[2001:db8::1]/updates"})
        expect (detail::isSafeHttpUrl (good), std::string ("absolute HTTP(S) accepted: ") + good);
    for (const auto bad : {"", "//example.test", "example.test", "file:///tmp/update", "javascript:alert(1)",
             "https://", "https:///file", "https://example.test/\nother", "https://example.test/ with-space",
             "https://[:::]/", "https://[1:2:3]/", "https://[2001::db8::1]/"})
        expect (! detail::isSafeHttpUrl (bad), "unsafe/non-absolute destination rejected");

    const auto plain = detail::parseManifest ("0.4,https://example.test/target-from-B1\r\n");
    expect (plain && plain->version == "0.4" && plain->pageUrl == "https://example.test/target-from-B1",
            "A1/B1 simple CSV uses the actual B1 target");
    const auto quoted = detail::parseManifest ("\xef\xbb\xbf\"0.4.0\",\"https://example.test/p?q=a,b\"\r\n");
    expect (quoted && quoted->version == "0.4.0" && quoted->pageUrl == "https://example.test/p?q=a,b",
            "UTF-8 BOM, quoted cells and embedded comma are parsed together");
    std::vector<std::string> badCsv {"", "0.4", "0.4,", "0.4,file:///local", "0.3-beta,https://example.test/",
        "0.4,\"https://example.test/", "0.4,\"https://example.test/\"junk", "<html>login</html>",
        std::string ("0.4,https://example.test/") + char (0xff),
        std::string ("0.4,https://example.test/") + "\xed\xa0\x80",
        std::string ("0.4,https://example.test/") + std::string (1, '\0') + "hidden",
        std::string (65537, '1')};
    for (const auto& csv : badCsv)
        expect (! detail::parseManifest (csv), "invalid/unsafe/oversized CSV fails closed");
    expect (detail::maximumBodyBytes == 65536, "declared body cap is 64 KiB");
    expect (detail::maximumRequestTime == 8000ms, "production total request budget is eight seconds");
    std::cout << "PASS parser/version expectation group\n";
}

void manualLifecycleChecks()
{
    std::atomic<int> calls { 0 };
    std::atomic<bool> background { false };
    const auto foreground = std::this_thread::get_id();
    auto options = at ("https://unused.example.test/");
    options.fetch = [&] (const juce::String&, detail::RequestControl&) {
        ++calls;
        background = std::this_thread::get_id() != foreground;
        detail::FetchResponse response;
        response.success = true;
        response.statusCode = 200;
        response.contentType = "text/csv";
        response.body = "0.4,https://example.test/from-B1\n";
        return response;
    };
    UpdateChecker checker ("0.3.0", options);
    std::this_thread::sleep_for (50ms);
    for (int i = 0; i < 20; ++i)
    {
        expect (checker.result().state == UpdateState::idle, "constructor and cached polling remain idle");
        expect (checker.installedVersion() == "0.3.0", "installed version is retained");
    }
    expect (calls == 0, "construction/polling makes no network or fetch call");
    expect (checker.checkForUpdates(), "explicit manual request starts");
    auto value = waitDone (checker);
    expect (calls == 1 && background, "fetch runs exactly once on a background thread");
    expect (value.state == UpdateState::available && value.pageUrl == "https://example.test/from-B1", "manual result uses B1");
    expect (checker.checkForUpdates(), "a completed check can be retried explicitly");
    waitDone (checker);
    expect (calls == 2, "retry makes one additional request");
    UpdateChecker released ("2.7.0", options);
    expect (released.installedVersion() == "2.7.0", "installed version comes from owner, not a hard-coded 0.3");
    released.checkForUpdates();
    expect (waitDone (released).state == UpdateState::upToDate, "older remote is compared with supplied installed version");
    std::cout << "PASS manual lifecycle expectation group\n";
}

void startupOnceChecks()
{
    struct Gate
    {
        std::atomic<int> calls { 0 };
        std::atomic<bool> open { false };
        bool valid = true;
    };
    const auto optionsFor = [] (const std::shared_ptr<Gate>& gate)
    {
        auto options = at ("https://unused.example.test/");
        options.fetch = [gate] (const juce::String&, detail::RequestControl& control)
        {
            ++gate->calls;
            while (! gate->open.load() && ! control.stopRequested())
                control.waitFor (2ms);
            detail::FetchResponse response;
            if (! control.stopRequested())
            {
                response.success = true;
                response.statusCode = 200;
                response.contentType = "text/csv";
                response.body = gate->valid ? "0.4,https://example.test/dynamic-B1\n" : "invalid,\n";
            }
            return response;
        };
        return options;
    };
    const auto entered = [] (const std::shared_ptr<Gate>& gate)
    {
        const auto until = Clock::now() + 1500ms;
        while (gate->calls.load() == 0 && Clock::now() < until)
            std::this_thread::sleep_for (1ms);
        expect (gate->calls.load() > 0, "test fetch reached its controlled gate");
    };
    {
        auto gate = std::make_shared<Gate>();
        UpdateChecker checker ("0.3.1", optionsFor (gate));
        expect (checker.result().state == UpdateState::idle && gate->calls == 0,
                "processor/scanner construction still performs no automatic fetch");
        expect (checker.checkOnStartup(), "first editor-startup hook starts exactly one check");
        entered (gate);
        expect (checker.result().state == UpdateState::checking, "editor startup immediately exposes checking state");
        expect (! checker.checkOnStartup() && ! checker.checkForUpdates(), "reopen and manual click share the in-flight gate");
        gate->open = true;
        expect (waitDone (checker).state == UpdateState::available, "startup check reaches available");
        for (int i = 0; i < 8; ++i)
            expect (! checker.checkOnStartup(), "reopening the same editor owner never repeats auto-check");
        expect (gate->calls == 1, "only one fetch was observed across startup/reopen attempts");
        expect (checker.checkForUpdates(), "manual retry remains available after the one startup attempt");
        waitDone (checker);
        expect (gate->calls == 2, "manual retry makes one additional request");
        expect (! checker.checkOnStartup(), "manual retry does not reset the startup-once marker");

        auto independentGate = std::make_shared<Gate>();
        independentGate->open = true;
        UpdateChecker independent ("0.3.1", optionsFor (independentGate));
        expect (independent.checkOnStartup(), "a fresh processor/checker gets its own startup attempt");
        expect (waitDone (independent).state == UpdateState::available && independentGate->calls == 1,
                "startup marker is per instance, not a global singleton");
    }
    {
        auto gate = std::make_shared<Gate>();
        gate->valid = false;
        gate->open = true;
        UpdateChecker checker ("0.3.1", optionsFor (gate));
        checker.checkOnStartup();
        expect (waitDone (checker).state == UpdateState::failed, "failed startup reaches a terminal cached failure");
        expect (! checker.checkOnStartup() && gate->calls == 1, "failure does not auto-retry on editor reopen");
        expect (checker.checkForUpdates(), "manual retry remains possible after failed startup");
        expect (waitDone (checker).state == UpdateState::failed && gate->calls == 2, "failed manual retry is one explicit request");
    }
    {
        auto gate = std::make_shared<Gate>();
        UpdateChecker checker ("0.3.1", optionsFor (gate));
        checker.checkForUpdates();
        entered (gate);
        expect (! checker.checkOnStartup(), "startup hook does not parallel an already running manual request");
        gate->open = true;
        waitDone (checker);
        expect (! checker.checkOnStartup() && gate->calls == 1, "a busy first startup attempt is consumed permanently");
    }
    {
        auto gate = std::make_shared<Gate>();
        UpdateChecker checker ("0.3.1", optionsFor (gate));
        checker.checkOnStartup();
        entered (gate);
        checker.cancel();
        waitDone (checker);
        expect (! checker.checkOnStartup(), "cancelled startup cannot restart automatically");
        gate->open = true;
        expect (checker.checkForUpdates(), "manual retry after cancellation is allowed");
        expect (waitDone (checker).state == UpdateState::available && gate->calls == 2,
                "manual retry receives a fresh request token after cancellation");
    }
    for (const auto includeManual : {false, true})
    {
        auto gate = std::make_shared<Gate>();
        UpdateChecker checker ("0.3.1", optionsFor (gate));
        std::atomic<bool> start { false };
        std::atomic<int> accepted { 0 };
        std::vector<std::thread> callers;
        for (int i = 0; i < 16; ++i)
            callers.emplace_back ([&] { while (! start.load()) std::this_thread::yield();
                                        if (checker.checkOnStartup()) ++accepted; });
        if (includeManual)
            callers.emplace_back ([&] { while (! start.load()) std::this_thread::yield();
                                        if (checker.checkForUpdates()) ++accepted; });
        start = true;
        for (auto& thread : callers) thread.join();
        entered (gate);
        expect (accepted == 1 && gate->calls == 1, "concurrent startup/manual callers still start one worker");
        gate->open = true;
        waitDone (checker);
        expect (! checker.checkOnStartup() && gate->calls == 1, "concurrent startup attempts consume one durable marker");
    }
    std::cout << "PASS first-editor startup-once expectation group\n";
}

void httpChecks (const std::string& base, const std::string& tls)
{
    const auto validBefore = count (base, "/valid");
    {
        UpdateChecker noAuto ("0.3.0", at (base + "/valid"));
        std::this_thread::sleep_for (80ms);
        expect (noAuto.result().state == UpdateState::idle, "real transport is idle until explicit check");
    }
    expect (count (base, "/valid") == validBefore, "constructor/destructor did not contact local server");

    struct Case { const char* path; UpdateState state; };
    for (const auto test : std::vector<Case> {{"/valid", UpdateState::available}, {"/equal", UpdateState::upToDate},
             {"/older", UpdateState::upToDate}, {"/numeric-newer", UpdateState::available},
             {"/quoted", UpdateState::available}, {"/absolute-redirect", UpdateState::available},
             {"/redirect/3", UpdateState::available}, {"/redirect/4", UpdateState::failed},
             {"/loop", UpdateState::failed}, {"/invalid-utf8", UpdateState::failed},
             {"/malformed", UpdateState::failed}, {"/empty-url", UpdateState::failed},
             {"/unsafe-url", UpdateState::failed}, {"/oversize", UpdateState::failed},
             {"/truncated", UpdateState::failed}, {"/html", UpdateState::failed}, {"/http-error", UpdateState::failed}})
    {
        UpdateChecker checker ("0.3.0", at (base + test.path, 1800ms));
        expect (checker.checkForUpdates(), std::string ("manual native HTTP starts: ") + test.path);
        const auto result = waitDone (checker, 4200ms);
        expect (result.state == test.state, std::string ("native HTTP expected outcome: ") + test.path);
        if (result.state == UpdateState::available)
            expect (result.pageUrl.startsWith (juce::String (base + "/landing")), "actual dynamic B1 returned without substitution");
    }
    expect (count (base, "/loop") <= 4, "redirect loop stops within three redirects");
    expect (count (base, "/landing") == 0, "checking never fetches the update page from B1");
    for (const auto path : {"/body-at-cap", "/oversize-no-length"})
    {
        detail::RequestControl control (Clock::now() + 3000ms);
        const auto response = detail::fetchHttp (juce::String (base + path), control);
        const auto exact = std::string (path) == "/body-at-cap";
        expect (response.body.size() <= 65536, "native response buffer never exceeds 64 KiB");
        expect (exact ? (response.success && response.body.size() == 65536) : ! response.success,
                exact ? "exact cap is accepted by transport" : "oversized streaming body without length is rejected");
    }

    const auto duplicatesBefore = count (base, "/duplicate");
    {
        UpdateChecker checker ("0.3.0", at (base + "/duplicate"));
        expect (checker.checkForUpdates(), "first duplicate-click request accepted");
        std::atomic<int> additionalStarts { 0 };
        std::vector<std::thread> clickers;
        for (int i = 0; i < 10; ++i)
            clickers.emplace_back ([&] { if (checker.checkForUpdates()) ++additionalStarts; });
        for (auto& thread : clickers)
            thread.join();
        expect (additionalStarts == 0, "concurrent repeat clicks do not create parallel workers");
        const auto pollStart = Clock::now();
        for (int i = 0; i < 200; ++i)
            checker.result();
        expect (elapsedMs (pollStart) < 500.0, "result polling does not wait for the remote response");
        expect (waitDone (checker).state == UpdateState::available, "one in-flight request completes normally");
    }
    expect (count (base, "/duplicate") == duplicatesBefore + 1, "server observed only one request for repeated clicks");

    {
        UpdateChecker flip ("0.3.0", at (base + "/flip"));
        flip.checkForUpdates();
        expect (waitDone (flip).state == UpdateState::available, "first result can offer update");
        flip.checkForUpdates();
        expect (flip.result().state == UpdateState::checking, "retry immediately publishes checking state");
        expect (waitDone (flip).state == UpdateState::failed, "invalid retry does not preserve an available state");
    }

    for (const auto path : {"/headers-stall", "/drip"})
    {
        UpdateChecker checker ("0.3.0", at (base + path));
        const auto start = Clock::now();
        checker.checkForUpdates();
        const auto value = waitDone (checker);
        const auto duration = elapsedMs (start);
        std::cout << "METRIC total_deadline path=" << path << " elapsed_ms=" << duration << '\n';
        expect (value.state == UpdateState::failed, "stalled response terminates in generic failure");
        expect (duration < 9800.0, "production eight-second total budget includes blocked/slow reads");
    }
    {
        UpdateChecker handshake ("0.3.0", at (tls + "/connect-stall", 500ms));
        const auto start = Clock::now();
        handshake.checkForUpdates();
        expect (waitDone (handshake, 2500ms).state == UpdateState::failed, "silent TLS connection setup is cancelled");
        const auto duration = elapsedMs (start);
        std::cout << "METRIC silent_tls elapsed_ms=" << duration << '\n';
        expect (duration < 2000.0, "connection-stage operation remains bounded");
        if (duration < 350.0)
            std::cout << "NOTE TLS failed before the deliberate stall; this case proves bounded failure, not a stalled TLS handshake\n";
    }

    for (const auto path : {"/headers-stall", "/body-stall"})
    {
        auto checker = std::make_unique<UpdateChecker> ("0.3.0", at (base + path));
        checker->checkForUpdates();
        std::this_thread::sleep_for (80ms);
        const auto start = Clock::now();
        checker.reset();
        const auto duration = elapsedMs (start);
        std::cout << "METRIC destructor_cancel path=" << path << " elapsed_ms=" << duration << '\n';
        expect (duration < 1800.0, "destruction cancels pending native operation and drains callbacks promptly");
    }
    {
        UpdateChecker waiting ("0.3.0", at (base + "/body-stall"));
        UpdateChecker independent ("0.3.0", at (base + "/valid"));
        waiting.checkForUpdates();
        independent.checkForUpdates();
        expect (waitDone (independent, 2500ms).state == UpdateState::available, "a stalled instance does not block a second instance");
        const auto start = Clock::now();
        waiting.cancel();
        waiting.cancel();
        expect (waitDone (waiting, 2500ms).state != UpdateState::available, "cancelled instance cannot publish stale success");
        expect (elapsedMs (start) < 1800.0, "explicit repeated cancellation wakes the in-flight request");
    }
    std::cout << "PASS native controlled HTTP expectation group\n";
}

void dllChecks (const std::string& base, const juce::String& path)
{
    using Make = void* (*) (const char*);
    using Start = bool (*) (void*);
    using Destroy = void (*) (void*);
    double maximum = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        HMODULE module = LoadLibraryW (path.toWideCharPointer());
        if (module == nullptr)
            throw std::runtime_error ("could not load independent update-checker DLL fixture");
        const auto make = reinterpret_cast<Make> (GetProcAddress (module, "ReviewerCreateChecker"));
        const auto start = reinterpret_cast<Start> (GetProcAddress (module, "ReviewerStartChecker"));
        const auto destroy = reinterpret_cast<Destroy> (GetProcAddress (module, "ReviewerDestroyChecker"));
        if (make == nullptr || start == nullptr || destroy == nullptr)
            throw std::runtime_error ("DLL fixture export mismatch");
        const auto endpoint = base + (i % 2 == 0 ? "/headers-stall" : "/body-stall");
        auto* checker = make (endpoint.c_str());
        expect (checker != nullptr && start (checker), "checker starts within a separately loaded DLL");
        std::this_thread::sleep_for (i % 3 == 0 ? 0ms : 35ms);
        const auto since = Clock::now();
        destroy (checker);
        expect (FreeLibrary (module) != 0, "DLL is released after checker destruction");
        maximum = std::max (maximum, elapsedMs (since));
        std::this_thread::sleep_for (20ms);
    }
    std::cout << "METRIC DLL_destroy_and_unload_max_ms=" << maximum << " iterations=16\n";
    expect (maximum < 1800.0, "native callbacks drain before actual DLL unload without forced thread termination");
    std::cout << "PASS DLL callback lifetime expectation group\n";
}

void liveCheck()
{
    UpdateChecker checker ("0.3.1");
    expect (checker.result().state == UpdateState::idle, "production endpoint remains idle until explicit CLI action");
    const auto start = Clock::now();
    expect (checker.checkForUpdates(), "explicit live read-only check starts");
    const auto result = waitDone (checker);
    std::cout << "METRIC explicit_live state=" << static_cast<int> (result.state)
              << " latest=" << result.latestVersion << " page=" << result.pageUrl
              << " elapsed_ms=" << elapsedMs (start) << '\n';
    expect (result.state == UpdateState::available || result.state == UpdateState::upToDate,
            "public CSV is fetched and validated by actual production transport");
}
} // namespace

int main (int argc, char** argv)
{
    std::cout << std::unitbuf;
    std::string base, tls;
    juce::String dll;
    bool live = false, liveOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string argument (argv[i]);
        if (argument == "--base" && i + 1 < argc) base = argv[++i];
        else if (argument == "--tls" && i + 1 < argc) tls = argv[++i];
        else if (argument == "--dll" && i + 1 < argc) dll = juce::String::fromUTF8 (argv[++i]);
        else if (argument == "--live") live = true;
        else if (argument == "--live-only") live = liveOnly = true;
        else return 2;
    }
    if ((! base.empty() && tls.empty()) || (dll.isNotEmpty() && base.empty()))
        return 2;
    try
    {
        if (! liveOnly)
        {
            parserChecks();
            manualLifecycleChecks();
            startupOnceChecks();
            if (! base.empty()) httpChecks (base, tls);
            if (dll.isNotEmpty()) dllChecks (base, dll);
        }
        if (live) liveCheck();
    }
    catch (const std::exception& error)
    {
        expect (false, std::string ("unhandled check exception: ") + error.what());
    }
    std::cout << "RESULT update_assertions=" << assertions << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
