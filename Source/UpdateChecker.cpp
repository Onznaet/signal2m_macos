#include "UpdateChecker.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace signal2m
{
namespace update_detail
{
namespace
{
bool validUtf8(std::string_view text)
{
    for (std::size_t i = 0; i < text.size();)
    {
        const auto byte = static_cast<unsigned char>(text[i++]);
        if (byte < 0x80)
        {
            if (byte == 0)
                return false;
            continue;
        }
        unsigned remaining = 0;
        unsigned char low = 0x80, high = 0xbf;
        if (byte >= 0xc2 && byte <= 0xdf)
            remaining = 1;
        else if (byte >= 0xe0 && byte <= 0xef)
        {
            remaining = 2;
            if (byte == 0xe0) low = 0xa0;
            if (byte == 0xed) high = 0x9f;
        }
        else if (byte >= 0xf0 && byte <= 0xf4)
        {
            remaining = 3;
            if (byte == 0xf0) low = 0x90;
            if (byte == 0xf4) high = 0x8f;
        }
        else
            return false;
        if (remaining > text.size() - i)
            return false;
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < low || first > high)
            return false;
        while (--remaining > 0)
        {
            const auto continuation = static_cast<unsigned char>(text[i++]);
            if (continuation < 0x80 || continuation > 0xbf)
                return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

char asciiLower(char value)
{
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

bool validIpv4(std::string_view address)
{
    unsigned groups = 0;
    while (!address.empty())
    {
        const auto dot = address.find('.');
        const auto group = address.substr(0, dot);
        if (group.empty()) return false;
        unsigned value = 0;
        for (char digit : group)
        {
            if (digit < '0' || digit > '9') return false;
            value = value * 10U + static_cast<unsigned>(digit - '0');
            if (value > 255U) return false;
        }
        ++groups;
        if (dot == std::string_view::npos) return groups == 4;
        address.remove_prefix(dot + 1);
    }
    return false;
}

bool validIpv6(std::string_view address)
{
    const auto countGroups = [](std::string_view side, bool ipv4Allowed) -> std::optional<unsigned>
    {
        if (side.empty()) return 0U;
        unsigned count = 0;
        while (!side.empty())
        {
            const auto colon = side.find(':');
            const auto group = side.substr(0, colon);
            if (group.empty()) return std::nullopt;
            if (group.find('.') != std::string_view::npos)
            {
                if (!ipv4Allowed || colon != std::string_view::npos || !validIpv4(group)) return std::nullopt;
                count += 2;
            }
            else
            {
                if (group.size() > 4) return std::nullopt;
                for (char character : group)
                    if (!((character >= '0' && character <= '9') || (asciiLower(character) >= 'a' && asciiLower(character) <= 'f')))
                        return std::nullopt;
                ++count;
            }
            if (colon == std::string_view::npos) return count;
            side.remove_prefix(colon + 1);
        }
        return std::nullopt;
    };
    const auto compression = address.find("::");
    if (compression == std::string_view::npos)
    {
        const auto count = countGroups(address, true);
        return count && *count == 8;
    }
    if (address.find("::", compression + 2) != std::string_view::npos) return false;
    const auto left = countGroups(address.substr(0, compression), false);
    const auto right = countGroups(address.substr(compression + 2), true);
    return left && right && *left + *right < 8;
}

std::optional<std::vector<std::uint32_t>> versionParts(std::string_view version)
{
    if (version.empty() || version.size() > maximumBodyBytes)
        return std::nullopt;
    std::vector<std::uint32_t> parts;
    std::uint32_t value = 0;
    bool digits = false;
    for (char character : version)
    {
        if (character == '.')
        {
            if (!digits)
                return std::nullopt;
            parts.push_back(value);
            value = 0;
            digits = false;
        }
        else if (character >= '0' && character <= '9')
        {
            const auto digit = static_cast<std::uint32_t>(character - '0');
            if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U)
                return std::nullopt;
            value = value * 10U + digit;
            digits = true;
        }
        else
            return std::nullopt;
    }
    if (!digits)
        return std::nullopt;
    parts.push_back(value);
    return parts;
}

bool acceptableContentType(const std::string& type)
{
    std::string lowered;
    for (char character : type)
        lowered += asciiLower(character);
    const auto separator = lowered.find(';');
    const auto mime = trim(std::string_view(lowered).substr(0, separator));
    if (mime != "text/csv" && mime != "application/csv" && mime != "text/plain")
        return false;
    const auto charset = lowered.find("charset=");
    if (charset != std::string::npos)
    {
        auto encoding = trim(std::string_view(lowered).substr(charset + 8));
        const auto next = encoding.find(';');
        encoding = trim(encoding.substr(0, next));
        if (encoding != "utf-8" && encoding != "\"utf-8\"")
            return false;
    }
    return true;
}
} // namespace

juce::String defaultEndpoint()
{
   #if defined(SIGNAL2M_UPDATE_FEED_URL)
    return SIGNAL2M_UPDATE_FEED_URL;
   #elif JUCE_MAC
    return "https://docs.google.com/spreadsheets/d/1ZyDHZskIvieqR_L6y4Ghu0gwvc6usByiNUF52y6AidM/export?format=csv&gid=640439888&range=A1%3AB1";
   #else
    return "https://docs.google.com/spreadsheets/d/1ZyDHZskIvieqR_L6y4Ghu0gwvc6usByiNUF52y6AidM/export?format=csv&gid=0&range=A1%3AB1";
   #endif
}

std::optional<int> compareVersions(std::string_view first, std::string_view second)
{
    const auto a = versionParts(first), b = versionParts(second);
    if (!a || !b)
        return std::nullopt;
    const auto count = std::max(a->size(), b->size());
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto left = i < a->size() ? (*a)[i] : 0U;
        const auto right = i < b->size() ? (*b)[i] : 0U;
        if (left != right)
            return left < right ? -1 : 1;
    }
    return 0;
}

bool isSafeHttpUrl(std::string_view url)
{
    if (url.empty() || url.size() > 8192 || !validUtf8(url))
        return false;
    for (std::size_t i = 0; i < url.size(); ++i)
    {
        const auto character = static_cast<unsigned char>(url[i]);
        if (character <= 0x20 || character == 0x7f || character == '\\' || character == '"' || character == '<' || character == '>')
            return false;
        if (character == '%')
        {
            const auto hex = [](char digit)
            {
                if (digit >= '0' && digit <= '9') return digit - '0';
                digit = asciiLower(digit);
                return digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 : -1;
            };
            if (i + 2 >= url.size()) return false;
            const int high = hex(url[i + 1]), low = hex(url[i + 2]);
            if (high < 0 || low < 0 || high * 16 + low < 0x20 || high * 16 + low == 0x7f)
                return false;
            i += 2;
        }
    }
    const auto separator = url.find("://");
    if (separator == std::string_view::npos)
        return false;
    std::string scheme(url.substr(0, separator));
    for (char& character : scheme) character = asciiLower(character);
    if (scheme != "http" && scheme != "https")
        return false;
    url.remove_prefix(separator + 3);
    const auto end = url.find_first_of("/?#");
    const auto authority = url.substr(0, end);
    if (authority.empty() || authority.find('@') != std::string_view::npos || authority.find('%') != std::string_view::npos)
        return false;
    std::string_view host = authority, port;
    if (authority.front() == '[')
    {
        const auto close = authority.find(']');
        if (close == std::string_view::npos || close < 3 || authority.substr(1, close - 1).find(':') == std::string_view::npos)
            return false;
        host = authority.substr(1, close - 1);
        if (!validIpv6(host)) return false;
        if (close + 1 < authority.size())
        {
            if (authority[close + 1] != ':') return false;
            port = authority.substr(close + 2);
            if (port.empty()) return false;
        }
    }
    else
    {
        const auto colon = authority.find(':');
        if (colon != std::string_view::npos)
        {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
            if (port.empty()) return false;
        }
        if (host.empty() || host.size() > 253)
            return false;
        if (host.back() == '.') host.remove_suffix(1);
        if (host.empty()) return false;
        bool firstInLabel = true;
        char previous = 0;
        for (unsigned char character : host)
        {
            if (character == '.')
            {
                if (firstInLabel || previous == '-') return false;
                firstInLabel = true;
            }
            else
            {
                const bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
                if (!(letter || (character >= '0' && character <= '9') || character == '-' || character >= 0x80)
                    || (firstInLabel && character == '-'))
                    return false;
                firstInLabel = false;
            }
            previous = static_cast<char>(character);
        }
        if (firstInLabel || previous == '-') return false;
    }
    if (!port.empty())
    {
        unsigned value = 0;
        for (char character : port)
        {
            if (character < '0' || character > '9') return false;
            value = value * 10U + static_cast<unsigned>(character - '0');
            if (value > 65535U) return false;
        }
        if (value == 0) return false;
    }
    return !host.empty();
}

std::optional<Manifest> parseManifest(std::string_view csv)
{
    if (csv.empty() || csv.size() > maximumBodyBytes || !validUtf8(csv))
        return std::nullopt;
    if (csv.size() >= 3 && csv.substr(0, 3) == std::string_view("\xef\xbb\xbf", 3))
        csv.remove_prefix(3);
    std::string first[2], field;
    std::size_t row = 0, column = 0;
    enum class State { start, unquoted, quoted, afterQuote };
    State state = State::start;
    const auto finishField = [&]
    {
        if (row == 0 && column < 2) first[column] = field;
        field.clear();
        ++column;
        state = State::start;
    };
    for (std::size_t i = 0; i < csv.size(); ++i)
    {
        const char character = csv[i];
        if (state == State::quoted)
        {
            if (character == '"') state = State::afterQuote;
            else if (row == 0 && column < 2) field += character;
            continue;
        }
        if (state == State::afterQuote && character == '"')
        {
            if (row == 0 && column < 2) field += '"';
            state = State::quoted;
            continue;
        }
        if (character == ',' || character == '\r' || character == '\n')
        {
            finishField();
            if (character != ',')
            {
                if (character == '\r')
                {
                    if (i + 1 >= csv.size() || csv[i + 1] != '\n') return std::nullopt;
                    ++i;
                }
                ++row;
                column = 0;
            }
            continue;
        }
        if (state == State::afterQuote)
            return std::nullopt;
        if (character == '"')
        {
            if (state != State::start) return std::nullopt;
            state = State::quoted;
        }
        else
        {
            if (static_cast<unsigned char>(character) < 0x20 && character != '\t') return std::nullopt;
            if (row == 0 && column < 2) field += character;
            state = State::unquoted;
        }
    }
    if (state == State::quoted)
        return std::nullopt;
    finishField();
    const auto version = trim(first[0]), url = trim(first[1]);
    if (!compareVersions(version, version) || !isSafeHttpUrl(url))
        return std::nullopt;
    return Manifest { juce::String::fromUTF8(version.data(), static_cast<int>(version.size())),
                      juce::String::fromUTF8(url.data(), static_cast<int>(url.size())) };
}

bool RequestControl::stopRequested() const noexcept
{
    return wasCancelled() || Clock::now() >= deadline;
}

void RequestControl::cancel() noexcept
{
    {
        // Use the wait mutex as well as the atomic: otherwise a notification
        // can be lost between a predicate check and entering wait_until.
        std::lock_guard<std::mutex> lock(mutex);
        cancelled.store(true, std::memory_order_release);
    }
    condition.notify_all();
}

bool RequestControl::waitFor(std::chrono::milliseconds duration)
{
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait_until(lock, std::min(deadline, Clock::now() + duration), [this] { return wasCancelled(); });
    return !stopRequested();
}
} // namespace update_detail

struct UpdateChecker::Impl
{
    Impl(juce::String version, update_detail::CheckerOptions configuration)
        : installed(std::move(version)), options(std::move(configuration))
    {
        if (options.endpoint.isEmpty()) options.endpoint = update_detail::defaultEndpoint();
        options.timeout = std::clamp(options.timeout, std::chrono::milliseconds(1), update_detail::maximumRequestTime);
        if (!options.fetch) options.fetch = update_detail::fetchHttp;
    }

    ~Impl()
    {
        std::lock_guard<std::mutex> launchLock(launchMutex);
        if (request) request->cancel();
        if (worker.joinable()) worker.join();
    }

    void perform(const std::shared_ptr<update_detail::RequestControl>& control)
    {
        UpdateResult completed;
        completed.state = UpdateState::failed;
        try
        {
            const auto response = options.fetch(options.endpoint, *control);
            if (!control->stopRequested() && response.success && response.statusCode == 200
                && response.body.size() <= update_detail::maximumBodyBytes
                && update_detail::acceptableContentType(response.contentType))
            {
                if (const auto manifest = update_detail::parseManifest(response.body))
                {
                    const auto comparison = update_detail::compareVersions(manifest->version.toStdString(), installed.toStdString());
                    if (comparison)
                    {
                        completed.state = *comparison > 0 ? UpdateState::available : UpdateState::upToDate;
                        completed.latestVersion = manifest->version;
                        completed.pageUrl = manifest->pageUrl;
                    }
                }
            }
        }
        catch (...)
        {
            completed = {};
            completed.state = UpdateState::failed;
        }
        if (control->wasCancelled()) completed = {};
        {
            std::lock_guard<std::mutex> lock(resultMutex);
            cached = std::move(completed);
            running.store(false, std::memory_order_release);
        }
    }

    const juce::String installed;
    update_detail::CheckerOptions options;
    std::mutex launchMutex;
    mutable std::mutex resultMutex;
    UpdateResult cached;
    std::atomic<bool> running { false };
    std::atomic<bool> startupAttempted { false };
    std::shared_ptr<update_detail::RequestControl> request;
    std::thread worker;
};

UpdateChecker::UpdateChecker(juce::String version) : UpdateChecker(std::move(version), {}) {}
UpdateChecker::UpdateChecker(juce::String version, update_detail::CheckerOptions options)
    : impl_(std::make_unique<Impl>(std::move(version), std::move(options))) {}
UpdateChecker::~UpdateChecker() = default;

bool UpdateChecker::checkOnStartup()
{
    // Mark the attempt before entering the normal launch gate. A manual request
    // already in flight consumes startup too, without creating parallel work.
    if (impl_->startupAttempted.exchange(true, std::memory_order_acq_rel))
        return false;
    return checkForUpdates();
}

bool UpdateChecker::checkForUpdates()
{
    std::lock_guard<std::mutex> launchLock(impl_->launchMutex);
    if (impl_->running.load(std::memory_order_acquire)) return false;
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->request = std::make_shared<update_detail::RequestControl>(update_detail::RequestControl::Clock::now() + impl_->options.timeout);
    {
        std::lock_guard<std::mutex> lock(impl_->resultMutex);
        impl_->cached = {};
        impl_->cached.state = UpdateState::checking;
    }
    impl_->running.store(true, std::memory_order_release);
    try
    {
        impl_->worker = std::thread([owner = impl_.get(), request = impl_->request] { owner->perform(request); });
    }
    catch (...)
    {
        impl_->running.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(impl_->resultMutex);
        impl_->cached.state = UpdateState::failed;
        return false;
    }
    return true;
}

UpdateResult UpdateChecker::result() const
{
    std::lock_guard<std::mutex> lock(impl_->resultMutex);
    return impl_->cached;
}

juce::String UpdateChecker::installedVersion() const { return impl_->installed; }

void UpdateChecker::cancel()
{
    std::lock_guard<std::mutex> launchLock(impl_->launchMutex);
    if (impl_->request) impl_->request->cancel();
}
} // namespace signal2m
