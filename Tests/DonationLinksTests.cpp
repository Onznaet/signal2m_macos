#include "DonationLinks.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;
using signal2m::DonationLinks;
using signal2m::DonationResult;
using signal2m::DonationState;
namespace detail = signal2m::donation_detail;
namespace transport = signal2m::update_detail;
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Predicate>
void waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2000ms)
{
    const auto deadline = Clock::now() + timeout;
    while (!predicate())
    {
        if (Clock::now() >= deadline) throw std::runtime_error("Timed out waiting for a donation test condition");
        std::this_thread::sleep_for(1ms);
    }
}

bool emptyLinks(const DonationResult& result)
{
    return result.urls[0].isEmpty() && result.urls[1].isEmpty();
}

DonationResult terminal(DonationLinks& links, std::chrono::milliseconds timeout = 2000ms)
{
    waitUntil([&] { return links.result().state != DonationState::loading; }, timeout);
    return links.result();
}

transport::FetchResponse csvResponse(std::string body = "https://example.org/first\r\nhttps://example.net/second\r\n")
{
    return { true, 200, "text/csv;charset=utf-8", std::move(body) };
}

std::string quoteCell(const std::string& value)
{
    std::string result = "\"";
    for (char character : value)
    {
        result += character;
        if (character == '"') result += '"';
    }
    return result + '"';
}

void testConfiguredFeed()
{
    const auto endpoint = detail::defaultEndpoint();
   #if defined(SIGNAL2M_DONATION_FEED_URL)
    require(endpoint == SIGNAL2M_DONATION_FEED_URL, "Donation loader must use the configured build endpoint");
   #else
    require(endpoint == "https://docs.google.com/spreadsheets/d/1ZyDHZskIvieqR_L6y4Ghu0gwvc6usByiNUF52y6AidM/export?format=csv&gid=1525027926&range=A1%3AA2",
            "The fallback must read only Don A1:A2 over HTTPS");
   #endif
}

void testCsvEmptyAndIndependentSlots()
{
    for (const std::string& csv : { std::string {}, std::string("\xef\xbb\xbf", 3), std::string("\r\n"),
                                   std::string("\"\"\r\n\"\"\r\n"), std::string(" \t\r\n \t\r\n") })
    {
        const auto links = detail::parseLinks(csv);
        require(links && (*links)[0].isEmpty() && (*links)[1].isEmpty(), "An empty sheet must be a valid empty list");
    }
    const auto both = detail::parseLinks("\xef\xbb\xbf\"https://example.org/?a=1,b=2\"\r\n\"https://example.net/second\"\r\n");
    require(both && (*both)[0] == "https://example.org/?a=1,b=2" && (*both)[1] == "https://example.net/second",
            "BOM, CRLF, quoted commas and both vertical cells must parse unchanged");
    const auto second = detail::parseLinks("\r\nhttps://example.net/only-second\r\n");
    require(second && (*second)[0].isEmpty() && (*second)[1] == "https://example.net/only-second",
            "Blank A1 may not shift the A2 link into the first slot");
    const auto first = detail::parseLinks("https://example.org/only-first\n");
    require(first && (*first)[0] == "https://example.org/only-first" && (*first)[1].isEmpty(),
            "Missing or empty A2 must preserve only the first slot");
    const auto unicode = detail::parseLinks(u8" https://example.org/помощь \n\thttps://example.net/a%20b\t\n");
    require(unicode && (*unicode)[0] == juce::String::fromUTF8(u8"https://example.org/помощь")
            && (*unicode)[1] == "https://example.net/a%20b", "UTF-8 URLs and surrounding cell whitespace must be handled explicitly");
    const auto multiline = detail::parseLinks("\"https://example.org/\ninvalid-path\"\r\nhttps://example.net/safe\r\n");
    require(multiline && (*multiline)[0].isEmpty() && (*multiline)[1] == "https://example.net/safe",
            "A quoted multiline unsafe URL must not invalidate the other row");
    const auto escaped = detail::parseLinks("\"https://example.org/\"\"unsafe\"\"\"\nhttps://example.net/safe");
    require(escaped && (*escaped)[0].isEmpty() && (*escaped)[1] == "https://example.net/safe",
            "Escaped CSV quotes must be decoded before validating each URL independently");
}

void testUnsafeUrlsAndStrictCsvBounds()
{
    const std::string safe = "https://example.net/safe";
    const auto safeUrl = juce::String::fromUTF8(safe.data(), static_cast<int>(safe.size()));
    for (const std::string unsafe : { "javascript:alert(1)", "file:///tmp/donate", "data:text/html,test", "ftp://example.org/",
                                     "//example.org/", "/relative", "https:///empty", "https://user:secret@example.org/",
                                     "https://example.org/a b", "https://example.org/%0d%0aInjected", "https://example.org/%00",
                                     "https://example.org/%7f", "https://example.org/%zz", "https://example.org\\@evil.org/",
                                     "https://[:::]/", "https://example.org:65536/", "<a href='https://example.org/'>donate</a>" })
    {
        const auto left = detail::parseLinks(quoteCell(unsafe) + "\r\n" + quoteCell(safe));
        const auto right = detail::parseLinks(quoteCell(safe) + "\n" + quoteCell(unsafe));
        require(left && (*left)[0].isEmpty() && (*left)[1] == safeUrl,
                "An unsafe A1 URL must hide only the first button");
        require(right && (*right)[0] == safeUrl && (*right)[1].isEmpty(),
                "An unsafe A2 URL must hide only the second button");
    }
    for (const char* body : { "\"https://example.org/", "https://example.org/\"", "\"https://example.org/\"junk",
                              "https://example.org/\r", "https://example.org/,https://example.net/", "\"\",\"\"\n",
                              "https://example.org/\nhttps://example.net/\nhttps://unexpected.example/", "\n\n\"unterminated",
                              "https://example.org/\n\x01" })
        require(!detail::parseLinks(body), "Malformed quoting, record endings, table shape and controls must fail");
    const auto bothUnsafe = detail::parseLinks("javascript:alert(1)\nfile:///tmp/donate");
    require(bothUnsafe && (*bothUnsafe)[0].isEmpty() && (*bothUnsafe)[1].isEmpty(),
            "Two unsafe URLs must produce a valid list with no visible slots");
    require(!detail::parseLinks(std::string("https://example.org/\0suffix\n", 28)), "Embedded NUL may not truncate validation");
    for (const auto& invalid : { std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
                                 std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2), std::string("\x80", 1) })
        require(!detail::parseLinks("https://example.org/" + invalid + "\n" + safe),
                "Invalid UTF-8 must fail before converting either cell to JUCE strings");
    require(detail::parseLinks(std::string(transport::maximumBodyBytes, ' ')).has_value(),
            "Exactly the body cap of blank data must remain bounded and valid");
    require(!detail::parseLinks(std::string(transport::maximumBodyBytes + 1U, ' ')),
            "The parser must enforce its body bound independently of transport");
    const auto tooLongUrl = detail::parseLinks("https://example.org/" + std::string(8200, 'a') + "\n" + safe);
    require(tooLongUrl && (*tooLongUrl)[0].isEmpty() && (*tooLongUrl)[1] == safeUrl,
            "An overlong unsafe URL must hide only its own bounded slot");
}

void testExplicitRefreshDuplicateAndCache()
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto release = std::make_shared<std::atomic<bool>>(false);
    transport::CheckerOptions options;
    options.fetch = [calls, release](const juce::String& endpoint, transport::RequestControl& control)
    {
        require(endpoint == detail::defaultEndpoint(), "Fetch may read only the feed, never its donation destinations");
        ++*calls;
        while (!release->load() && !control.stopRequested()) control.waitFor(5ms);
        return csvResponse();
    };
    DonationLinks links(options);
    require(links.result().state == DonationState::idle && emptyLinks(links.result()), "Construction must expose an empty idle cache");
    for (int i = 0; i < 100; ++i) links.result();
    std::this_thread::sleep_for(20ms);
    require(calls->load() == 0, "Construction and polling must not create background requests");
    require(links.refresh(), "A card opening must explicitly start a refresh");
    require(links.result().state == DonationState::loading && emptyLinks(links.result()), "Refresh must synchronously publish loading with cleared URLs");
    waitUntil([&] { return calls->load() == 1; });
    const auto busyStart = Clock::now();
    for (int i = 0; i < 2000; ++i) require(!links.refresh(), "Repeated refresh while loading must not create another worker");
    require(Clock::now() - busyStart < 500ms, "Busy refresh calls must not join or wait for the transport");
    release->store(true);
    const auto ready = terminal(links);
    require(ready.state == DonationState::ready && ready.urls[0] == "https://example.org/first"
            && ready.urls[1] == "https://example.net/second", "A ready cache must publish the two independent dynamic URLs together");
    std::this_thread::sleep_for(15ms);
    require(calls->load() == 1, "A cached result must not schedule another refresh automatically");
}

void testRefreshClearsStaleUrlsAndRetry()
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto release = std::make_shared<std::atomic<bool>>(false);
    transport::CheckerOptions options;
    options.fetch = [calls, release](const juce::String&, transport::RequestControl& control)
    {
        const int attempt = ++*calls;
        if (attempt == 2)
        {
            while (!release->load() && !control.stopRequested()) control.waitFor(5ms);
            auto failed = csvResponse();
            failed.statusCode = 503;
            return failed;
        }
        if (attempt == 3) return csvResponse("");
        if (attempt == 4) return csvResponse("\nhttps://example.net/recovered\n");
        return csvResponse();
    };
    DonationLinks links(options);
    require(links.refresh() && terminal(links).state == DonationState::ready, "Initial links must load normally");
    require(links.refresh(), "Each later card opening must allow a new explicit refresh");
    require(links.result().state == DonationState::loading && emptyLinks(links.result()), "An in-flight re-open must never keep stale links visible");
    release->store(true);
    const auto failed = terminal(links);
    require(failed.state == DonationState::failed && emptyLinks(failed), "A failed refresh must not restore URLs from an earlier success");
    require(links.refresh(), "Network failures must allow the next explicit retry");
    const auto blank = terminal(links);
    require(blank.state == DonationState::ready && emptyLinks(blank), "A successful blank sheet must clear both donation slots without an error");
    require(links.refresh(), "A blank success must still allow a later explicit refresh");
    const auto recovered = terminal(links);
    require(recovered.state == DonationState::ready && recovered.urls[0].isEmpty()
            && recovered.urls[1] == "https://example.net/recovered", "A later second-only URL must retain its original slot");
}

void testErrorsAndContentPolicy()
{
    for (int mode = 0; mode < 9; ++mode)
    {
        transport::CheckerOptions options;
        options.fetch = [mode](const juce::String&, transport::RequestControl&)
        {
            auto response = csvResponse();
            if (mode == 0) response.success = false;
            if (mode == 1) response.statusCode = 404;
            if (mode == 2) response.contentType = "text/html;charset=utf-8";
            if (mode == 3) response.contentType = "text/csv;charset=windows-1251";
            if (mode == 4) response.body = "\"unterminated";
            if (mode == 5) response.body = std::string(transport::maximumBodyBytes + 1U, 'a');
            if (mode == 6) response.body = std::string("\xc0\xaf", 2);
            if (mode == 7) throw std::runtime_error("Injected donation transport exception");
            if (mode == 8) response.contentType.clear();
            return response;
        };
        DonationLinks links(options);
        require(links.refresh(), "An injected failure must still start through the normal worker");
        const auto failed = terminal(links);
        require(failed.state == DonationState::failed && emptyLinks(failed), "Network, HTTP, HTML, encoding, bounds and parser errors must fail without URLs");
    }
    for (const std::string type : { "Text/Csv; Charset=\"UTF-8\"", "application/csv", "text/plain;charset=utf-8" })
    {
        transport::CheckerOptions options;
        options.fetch = [type](const juce::String&, transport::RequestControl&)
        {
            auto response = csvResponse();
            response.contentType = type;
            return response;
        };
        DonationLinks links(options);
        require(links.refresh() && terminal(links).state == DonationState::ready,
                "Supported CSV/plain UTF-8 content types must be accepted case-insensitively");
    }
}

void testConcurrentRefreshAndIndependentInstances()
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    auto release = std::make_shared<std::atomic<bool>>(false);
    transport::CheckerOptions options;
    options.fetch = [calls, release](const juce::String& endpoint, transport::RequestControl& control)
    {
        ++*calls;
        while (!release->load() && !control.stopRequested()) control.waitFor(5ms);
        return csvResponse(endpoint.contains("second-instance") ? "https://example.net/second-instance\n" : "https://example.org/first-instance\n");
    };
    DonationLinks first(options);
    std::atomic<bool> start { false };
    std::atomic<int> accepted { 0 };
    std::vector<std::thread> callers;
    for (int i = 0; i < 12; ++i)
        callers.emplace_back([&]
        {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            if (first.refresh()) ++accepted;
        });
    start.store(true, std::memory_order_release);
    for (auto& caller : callers) caller.join();
    waitUntil([&] { return calls->load() == 1; });
    require(accepted.load() == 1, "Concurrent refresh callbacks must share exactly one in-flight worker");
    options.endpoint = "https://example.org/second-instance-feed";
    DonationLinks second(options);
    require(second.refresh(), "A separate processor/card loader must have independent request state");
    waitUntil([&] { return calls->load() == 2; });
    first.cancel();
    require(first.result().state == DonationState::idle && emptyLinks(first.result()), "Cancellation must immediately clear the first instance cache");
    release->store(true);
    require(terminal(second).urls[0] == "https://example.net/second-instance", "Cancelling one loader may not cancel or overwrite another instance");
    waitUntil([&] { return first.refresh(); });
    require(terminal(first).urls[0] == "https://example.org/first-instance", "A cancelled loader must support a new independent refresh after draining");
}

void testTotalDeadlineCancelAndJoinedLifetime()
{
    auto calls = std::make_shared<std::atomic<int>>(0);
    transport::CheckerOptions options;
    options.timeout = 60ms;
    options.fetch = [calls](const juce::String&, transport::RequestControl& control)
    {
        if (++*calls == 1) control.waitFor(5s);
        return csvResponse();
    };
    DonationLinks timed(options);
    const auto start = Clock::now();
    require(timed.refresh(), "A timed request must start normally");
    const auto failed = terminal(timed);
    require(failed.state == DonationState::failed && emptyLinks(failed), "A response arriving after the total deadline must not publish URLs");
    require(Clock::now() - start >= 30ms && Clock::now() - start < 1000ms, "RequestControl must bound a stalled injected fetch by the total deadline");
    require(timed.refresh() && terminal(timed).state == DonationState::ready, "A timeout must not prevent a later successful refresh");

    auto entered = std::make_shared<std::atomic<int>>(0);
    options.timeout = 8s;
    options.fetch = [entered](const juce::String&, transport::RequestControl& control)
    {
        ++*entered;
        control.waitFor(5s);
        return csvResponse(); // A deliberately late valid payload must be discarded after cancel.
    };
    DonationLinks cancelled(options);
    require(cancelled.refresh(), "A cancellable request must launch");
    waitUntil([&] { return entered->load() == 1; });
    const auto cancelStart = Clock::now();
    cancelled.cancel();
    require(Clock::now() - cancelStart < 250ms, "cancel must signal transport without joining its worker");
    require(cancelled.result().state == DonationState::idle && emptyLinks(cancelled.result()), "Cancel must clear stale result URLs immediately");
    waitUntil([&] { return cancelled.refresh(); });
    waitUntil([&] { return entered->load() == 2; });
    cancelled.cancel();
    std::this_thread::sleep_for(15ms);
    require(cancelled.result().state == DonationState::idle && emptyLinks(cancelled.result()), "Cancelled late successful fetches may never restore links");

    auto finishing = std::make_shared<std::atomic<int>>(0);
    options.fetch = [entered, finishing](const juce::String&, transport::RequestControl& control)
    {
        ++*entered;
        control.waitFor(5s);
        ++*finishing;
        return csvResponse();
    };
    const int before = entered->load();
    const auto destructionStart = Clock::now();
    {
        auto scoped = std::make_unique<DonationLinks>(options);
        require(scoped->refresh(), "A short-lived loader must launch its normal worker");
        waitUntil([&] { return entered->load() > before; });
    }
    require(finishing->load() == 1 && Clock::now() - destructionStart < 1000ms,
            "Destruction must cancel and join cooperative transport before returning, with no detached lifetime");

    options.timeout = 60000ms;
    options.fetch = [](const juce::String&, transport::RequestControl& control)
    {
        const auto remaining = control.deadline - Clock::now();
        require(remaining <= 8000ms && remaining > 7500ms, "Configured request deadline must be capped at eight seconds");
        return csvResponse();
    };
    DonationLinks maximum(options);
    require(maximum.refresh() && terminal(maximum).state == DonationState::ready, "An oversized timeout option must use the bounded worker path");
}
} // namespace

int main(int argc, char** argv)
{
    std::cout << std::unitbuf;
    if (argc == 2 && std::string(argv[1]) == "--live")
    {
        transport::CheckerOptions options;
        options.endpoint = detail::defaultEndpoint();
        options.fetch = [](const juce::String& endpoint, transport::RequestControl& control)
        {
            auto response = transport::fetchHttp(endpoint, control);
            const bool parsed = response.success && detail::parseLinks(response.body).has_value();
            std::cout << "transport_success=" << response.success
                      << " statusCode=" << response.statusCode
                      << " nativeError=" << response.nativeError
                      << " secureFailureFlags=" << response.secureFailureFlags
                      << " stage=" << response.stage
                      << " body_bytes=" << response.body.size()
                      << " parse_attempted=" << response.success
                      << " parse_ok=" << parsed << '\n';
            return response;
        };
        DonationLinks links(std::move(options));
        if (!links.refresh()) return 1;
        const auto result = terminal(links, 10000ms);
        std::cout << "state=" << static_cast<int>(result.state)
                  << " A1_present=" << result.urls[0].isNotEmpty()
                  << " A2_present=" << result.urls[1].isNotEmpty() << '\n';
        return result.state == DonationState::ready ? 0 : 1;
    }
    struct Test { const char* name; void (*run)(); };
    const Test tests[] = {
        { "configured bounded A1:A2 donation feed", testConfiguredFeed },
        { "empty sheet, strict UTF-8 CSV and independent slots", testCsvEmptyAndIndependentSlots },
        { "unsafe URL isolation, malformed CSV and body bounds", testUnsafeUrlsAndStrictCsvBounds },
        { "explicit-only refresh, duplicate guard and passive cache", testExplicitRefreshDuplicateAndCache },
        { "refresh clears stale links, blank success and explicit retry", testRefreshClearsStaleUrlsAndRetry },
        { "network/data errors and UTF-8 content policy", testErrorsAndContentPolicy },
        { "concurrent refreshes and independent loader instances", testConcurrentRefreshAndIndependentInstances },
        { "total deadline, immediate cancel and joined destruction", testTotalDeadlineCancelAndJoinedLifetime }
    };
    int failed = 0;
    for (const auto& test : tests)
    {
        try
        {
            test.run();
            std::cout << "PASS " << test.name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failed;
            std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (failed == 0 ? "All DonationLinks checks passed.\n" : "DonationLinks checks failed.\n");
    return failed == 0 ? 0 : 1;
}
