// A minimal native dylib boundary for the independent lifetime loader.
// The loader itself does not link JUCE, Signal2M, or Objective-C transport code.
#include "UpdateChecker.h"
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <cstdint>
#include <utility>

#if !defined(SIGNAL2M_MAC_TRANSPORT_TEST_HOOKS) || !SIGNAL2M_MAC_TRANSPORT_TEST_HOOKS
 #error The independent fixture requires test-only hooks; never enable them for the product target
#endif

namespace signal2m::update_detail
{
std::uint64_t macUpdateActiveContexts() noexcept;
std::uint64_t macUpdateActiveCallbacks() noexcept;
std::uint64_t macUpdateTeardownFallbacks() noexcept;
std::uint64_t macUpdateTemporaryImagePins() noexcept;
std::uint64_t macUpdateDelegateClasses() noexcept;
bool macUpdateImagePermanentlyPinned() noexcept;
void macUpdateForceNextTeardownFallbackForTest() noexcept;
}

#define REVIEWER_EXPORT extern "C" __attribute__((visibility("default")))

REVIEWER_EXPORT void* ReviewerMacCreateChecker(const char* endpoint, std::uint32_t timeoutMilliseconds) noexcept
{
    try
    {
        if (!endpoint || timeoutMilliseconds == 0 || timeoutMilliseconds > 8000) return nullptr;
        signal2m::update_detail::CheckerOptions options;
        options.endpoint = juce::String::fromUTF8(endpoint);
        options.timeout = std::chrono::milliseconds(timeoutMilliseconds);
        return new signal2m::UpdateChecker("0.3.1", std::move(options));
    }
    catch (...) { return nullptr; }
}

REVIEWER_EXPORT bool ReviewerMacStartChecker(void* checker) noexcept
{
    try { return checker && static_cast<signal2m::UpdateChecker*>(checker)->checkForUpdates(); }
    catch (...) { return false; }
}

// Numeric values are the explicitly tested C boundary: idle=0, checking=1,
// upToDate=2, available=3, failed=4; 255 denotes a fixture API error.
REVIEWER_EXPORT unsigned ReviewerMacCheckerState(void* checker) noexcept
{
    try { return checker ? static_cast<unsigned>(static_cast<signal2m::UpdateChecker*>(checker)->result().state) : 255U; }
    catch (...) { return 255U; }
}

REVIEWER_EXPORT bool ReviewerMacDestroyChecker(void* checker) noexcept
{
    try { delete static_cast<signal2m::UpdateChecker*>(checker); return true; }
    catch (...) { return false; }
}

REVIEWER_EXPORT void ReviewerMacGetDiagnostics(std::uint64_t* contexts, std::uint64_t* callbacks,
                                               std::uint64_t* fallbacks, std::uint64_t* temporaryPins,
                                               std::uint64_t* delegateClasses,
                                               bool* permanentlyPinned) noexcept
{
    namespace detail = signal2m::update_detail;
    if (contexts) *contexts = detail::macUpdateActiveContexts();
    if (callbacks) *callbacks = detail::macUpdateActiveCallbacks();
    if (fallbacks) *fallbacks = detail::macUpdateTeardownFallbacks();
    if (temporaryPins) *temporaryPins = detail::macUpdateTemporaryImagePins();
    if (delegateClasses) *delegateClasses = detail::macUpdateDelegateClasses();
    if (permanentlyPinned) *permanentlyPinned = detail::macUpdateImagePermanentlyPinned();
}

REVIEWER_EXPORT bool ReviewerMacTransportIsDynamicImage() noexcept
{
    Dl_info information{};
    if (dladdr(reinterpret_cast<const void*>(&signal2m::update_detail::fetchHttp), &information) == 0
        || information.dli_fbase == nullptr) return false;
    const auto* header = static_cast<const mach_header*>(information.dli_fbase);
    return header->filetype == MH_DYLIB || header->filetype == MH_BUNDLE;
}

REVIEWER_EXPORT void ReviewerMacForceNextFallback() noexcept
{
    signal2m::update_detail::macUpdateForceNextTeardownFallbackForTest();
}
