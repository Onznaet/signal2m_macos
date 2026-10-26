#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace signal2m
{
enum class UpdateState { idle, checking, upToDate, available, failed };

struct UpdateResult
{
    UpdateState state = UpdateState::idle;
    juce::String latestVersion;
    juce::String pageUrl;
};

// Internal seams for bounded parsing and independent transport tests. Production
// owners use the single-argument UpdateChecker constructor and its fixed source.
namespace update_detail
{
inline constexpr std::size_t maximumBodyBytes = 64U * 1024U;
inline constexpr std::chrono::milliseconds maximumRequestTime { 8000 };
juce::String defaultEndpoint();

std::optional<int> compareVersions(std::string_view first, std::string_view second);
bool isSafeHttpUrl(std::string_view url);
struct Manifest
{
    juce::String version;
    juce::String pageUrl;
};
std::optional<Manifest> parseManifest(std::string_view utf8Csv);

struct RequestControl
{
    using Clock = std::chrono::steady_clock;
    explicit RequestControl(Clock::time_point end) : deadline(end) {}
    bool stopRequested() const noexcept;
    bool wasCancelled() const noexcept { return cancelled.load(std::memory_order_acquire); }
    void cancel() noexcept;
    bool waitFor(std::chrono::milliseconds duration);

    const Clock::time_point deadline;
    std::atomic<bool> cancelled { false };
    // Shared only by the background transport, native cancellation signals, and
    // cancellation notification. Never accessed by audio processing.
    std::mutex mutex;
    std::condition_variable condition;
};

struct FetchResponse
{
    bool success = false;
    int statusCode = 0;
    std::string contentType;
    std::string body;
    // Internal transport diagnostics for headless tests, never UI status text.
    std::uint32_t nativeError = 0;
    std::uint32_t secureFailureFlags = 0;
    std::string stage {};
};
FetchResponse fetchHttp(const juce::String& endpoint, RequestControl&);
using FetchFunction = std::function<FetchResponse(const juce::String&, RequestControl&)>;
struct CheckerOptions
{
    juce::String endpoint;
    std::chrono::milliseconds timeout = maximumRequestTime;
    FetchFunction fetch;
};
} // namespace update_detail

// No network and no worker thread in construction. The first-editor startup
// hook and explicit manual checks share one worker. The per-processor owner
// outlives its editor; polling never starts work. Destruction cancels and joins.
class UpdateChecker
{
public:
    explicit UpdateChecker(juce::String installedVersion);
    UpdateChecker(juce::String installedVersion, update_detail::CheckerOptions);
    ~UpdateChecker();
    // One attempt per checker, including a busy/failed/cancelled attempt. Call
    // from the first visible editor startup, never processor/audio construction.
    bool checkOnStartup();
    bool checkForUpdates();
    UpdateResult result() const;
    juce::String installedVersion() const;
    void cancel();

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace signal2m
