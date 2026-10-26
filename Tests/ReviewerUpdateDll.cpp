// A test-only DLL boundary for exercising native callback drain before unload.
#include "UpdateChecker.h"
#include <utility>

extern "C" __declspec(dllexport) void* ReviewerCreateChecker (const char* endpoint)
{
    signal2m::update_detail::CheckerOptions options;
    options.endpoint = juce::String::fromUTF8 (endpoint);
    return new signal2m::UpdateChecker ("0.3.0", std::move (options));
}

extern "C" __declspec(dllexport) bool ReviewerStartChecker (void* checker)
{
    return static_cast<signal2m::UpdateChecker*> (checker)->checkForUpdates();
}

extern "C" __declspec(dllexport) void ReviewerDestroyChecker (void* checker)
{
    delete static_cast<signal2m::UpdateChecker*> (checker);
}
