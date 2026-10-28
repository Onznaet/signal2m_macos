#pragma once

#include "UpdateChecker.h"

#include <array>
#include <memory>
#include <optional>
#include <string_view>

namespace signal2m
{
enum class DonationState { idle, loading, ready, failed };

struct DonationResult
{
    DonationState state = DonationState::idle;
    std::array<juce::String, 2> urls;
};

namespace donation_detail
{
juce::String defaultEndpoint();
// A1 and A2 retain their positions. Empty or unsafe URLs hide only that slot;
// invalid UTF-8, malformed CSV or a wider/nonempty longer table fails parsing.
std::optional<std::array<juce::String, 2>> parseLinks(std::string_view utf8Csv);
} // namespace donation_detail

// Construction and polling do no I/O and create no worker. Each explicit card
// opening calls refresh(); a busy loader declines immediately. Destruction
// cancels and joins its worker. These methods are never used by audio processing.
class DonationLinks
{
public:
    DonationLinks();
    explicit DonationLinks(update_detail::CheckerOptions);
    ~DonationLinks();

    bool refresh();
    DonationResult result() const;
    // Clear cached links immediately and request cancellation. A draining
    // request remains busy until joined safely by a later refresh/destruction.
    void cancel();

    DonationLinks(const DonationLinks&) = delete;
    DonationLinks& operator=(const DonationLinks&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace signal2m
