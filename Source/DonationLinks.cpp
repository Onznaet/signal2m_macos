#include "DonationLinks.h"

#include <algorithm>
#include <string>
#include <thread>
#include <utility>

namespace signal2m
{
namespace donation_detail
{
namespace
{
// Validate the complete byte stream before passing bounded cells to JUCE.
// JUCE 7's CharPointer_UTF8 validator does not reject every overlong/surrogate
// encoding, so use the same strict UTF-8 rules as the existing update parser.
bool validUtf8(std::string_view text)
{
    for (std::size_t i = 0; i < text.size();)
    {
        const auto byte = static_cast<unsigned char>(text[i++]);
        if (byte < 0x80)
        {
            if (byte == 0) return false;
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
        if (remaining > text.size() - i) return false;
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < low || first > high) return false;
        while (--remaining > 0)
        {
            const auto continuation = static_cast<unsigned char>(text[i++]);
            if (continuation < 0x80 || continuation > 0xbf) return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

bool acceptableContentType(std::string type)
{
    for (char& character : type)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character + ('a' - 'A'));
    const auto mime = trim(std::string_view(type).substr(0, type.find(';')));
    if (mime != "text/csv" && mime != "application/csv" && mime != "text/plain") return false;
    const auto charset = type.find("charset=");
    if (charset != std::string::npos)
    {
        auto encoding = trim(std::string_view(type).substr(charset + 8));
        encoding = trim(encoding.substr(0, encoding.find(';')));
        if (encoding != "utf-8" && encoding != "\"utf-8\"") return false;
    }
    return true;
}
} // namespace

juce::String defaultEndpoint()
{
   #if defined(SIGNAL2M_DONATION_FEED_URL)
    return SIGNAL2M_DONATION_FEED_URL;
   #else
    return "https://docs.google.com/spreadsheets/d/1ZyDHZskIvieqR_L6y4Ghu0gwvc6usByiNUF52y6AidM/export?format=csv&gid=1525027926&range=A1%3AA2";
   #endif
}

std::optional<std::array<juce::String, 2>> parseLinks(std::string_view csv)
{
    if (csv.size() > update_detail::maximumBodyBytes || !validUtf8(csv)) return std::nullopt;
    if (csv.size() >= 3 && csv.substr(0, 3) == std::string_view("\xef\xbb\xbf", 3)) csv.remove_prefix(3);

    std::array<std::string, 2> cells;
    std::string field;
    std::size_t row = 0;
    enum class State { start, unquoted, quoted, afterQuote };
    State state = State::start;
    const auto finishRow = [&]
    {
        if (row < cells.size()) cells[row] = std::move(field);
        else if (!trim(field).empty()) return false;
        field.clear();
        ++row;
        state = State::start;
        return true;
    };
    for (std::size_t i = 0; i < csv.size(); ++i)
    {
        const char character = csv[i];
        if (state == State::quoted)
        {
            if (character == '"') state = State::afterQuote;
            else field += character;
            continue;
        }
        if (state == State::afterQuote && character == '"')
        {
            field += '"';
            state = State::quoted;
            continue;
        }
        if (character == ',') return std::nullopt; // The configured range is one column.
        if (character == '\r' || character == '\n')
        {
            if (character == '\r')
            {
                if (i + 1 >= csv.size() || csv[i + 1] != '\n') return std::nullopt;
                ++i;
            }
            if (!finishRow()) return std::nullopt;
            continue;
        }
        if (state == State::afterQuote) return std::nullopt;
        if (character == '"')
        {
            if (state != State::start) return std::nullopt;
            state = State::quoted;
        }
        else
        {
            if (static_cast<unsigned char>(character) < 0x20 && character != '\t') return std::nullopt;
            field += character;
            state = State::unquoted;
        }
    }
    if (state == State::quoted || !finishRow()) return std::nullopt;

    std::array<juce::String, 2> links;
    for (std::size_t slot = 0; slot < cells.size(); ++slot)
    {
        const auto url = trim(cells[slot]);
        if (update_detail::isSafeHttpUrl(url))
            links[slot] = juce::String::fromUTF8(url.data(), static_cast<int>(url.size()));
    }
    return links;
}
} // namespace donation_detail

struct DonationLinks::Impl
{
    explicit Impl(update_detail::CheckerOptions configuration) : options(std::move(configuration))
    {
        if (options.endpoint.isEmpty()) options.endpoint = donation_detail::defaultEndpoint();
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
        DonationResult completed;
        completed.state = DonationState::failed;
        try
        {
            const auto response = options.fetch(options.endpoint, *control);
            if (!control->stopRequested() && response.success && response.statusCode == 200
                && response.body.size() <= update_detail::maximumBodyBytes
                && donation_detail::acceptableContentType(response.contentType))
            {
                if (auto links = donation_detail::parseLinks(response.body))
                {
                    completed.state = DonationState::ready;
                    completed.urls = std::move(*links);
                }
            }
        }
        catch (...)
        {
            completed = {};
            completed.state = DonationState::failed;
        }
        std::lock_guard<std::mutex> lock(resultMutex);
        // Check inside the publication lock so a concurrent cancel cannot be
        // followed by a late successful result restoring the cleared links.
        if (control->wasCancelled()) completed = {};
        else if (control->stopRequested())
        {
            completed = {};
            completed.state = DonationState::failed;
        }
        cached = std::move(completed);
        running.store(false, std::memory_order_release);
    }

    update_detail::CheckerOptions options;
    std::mutex launchMutex;
    mutable std::mutex resultMutex;
    DonationResult cached;
    std::atomic<bool> running { false };
    std::shared_ptr<update_detail::RequestControl> request;
    std::thread worker;
};

DonationLinks::DonationLinks() : DonationLinks(update_detail::CheckerOptions {}) {}
DonationLinks::DonationLinks(update_detail::CheckerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
DonationLinks::~DonationLinks() = default;

bool DonationLinks::refresh()
{
    if (impl_->running.load(std::memory_order_acquire)) return false;
    std::unique_lock<std::mutex> launchLock(impl_->launchMutex, std::try_to_lock);
    if (!launchLock.owns_lock() || impl_->running.load(std::memory_order_acquire)) return false;
    // running becomes false only at the end of perform(). An earlier finished
    // worker is joined before reuse; an in-flight request never reaches join.
    if (impl_->worker.joinable()) impl_->worker.join();
    {
        std::lock_guard<std::mutex> lock(impl_->resultMutex);
        impl_->cached = {};
        impl_->cached.state = DonationState::loading;
    }
    try
    {
        impl_->request = std::make_shared<update_detail::RequestControl>(update_detail::RequestControl::Clock::now() + impl_->options.timeout);
        impl_->running.store(true, std::memory_order_release);
        impl_->worker = std::thread([owner = impl_.get(), request = impl_->request] { owner->perform(request); });
    }
    catch (...)
    {
        impl_->running.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(impl_->resultMutex);
        impl_->cached = {};
        impl_->cached.state = DonationState::failed;
        return false;
    }
    return true;
}

DonationResult DonationLinks::result() const
{
    std::lock_guard<std::mutex> lock(impl_->resultMutex);
    return impl_->cached;
}

void DonationLinks::cancel()
{
    std::lock_guard<std::mutex> launchLock(impl_->launchMutex);
    if (impl_->request) impl_->request->cancel();
    std::lock_guard<std::mutex> lock(impl_->resultMutex);
    impl_->cached = {};
}
} // namespace signal2m
