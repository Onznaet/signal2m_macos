#include "UpdateChecker.h"

#if JUCE_MAC

#if !__has_feature(objc_arc)
 #error UpdateHttpTransportMac.mm must be compiled with Objective-C ARC enabled
#endif

#import <Foundation/Foundation.h>
#import <Security/SecProtocolTypes.h>
#include <dlfcn.h>
#import <objc/runtime.h>
#include <mach-o/loader.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>

namespace signal2m::update_detail::mac_transport
{
namespace
{
using Clock = RequestControl::Clock;
using namespace std::chrono_literals;
constexpr auto cancellationPoll = 5ms;
constexpr auto cleanupBudget = 250ms;
constexpr unsigned maximumRedirects = 3;
constexpr std::size_t maximumContentTypeBytes = 2048;

// A native teardown that misses its finite grace period fails closed. One
// retained loader reference protects any late Foundation callback, and further
// requests in this image fail before creating callbacks. It is never unpinned.
std::atomic<void*> permanentImagePin { nullptr };
std::atomic<bool> teardownUnavailable { false };
std::atomic<std::uint64_t> activeContexts { 0 }, activeCallbacks { 0 }, teardownFallbacks { 0 };
std::atomic<std::uint64_t> temporaryImagePins { 0 }, delegateClasses { 0 };
// Read-only diagnostics for native tests: no URL, headers, or response bytes.
// Bits: attempted, session, invalidated, queue-empty, callbacks-zero,
// session-gone, queue-gone, delegate-gone, network-success, quiescent,
// forced-fallback, worker-delegate-released.
std::atomic<std::uint32_t> lastTeardownFlags { 0 };
std::atomic<const char*> lastNetworkStage { "not-started" };
#if defined(SIGNAL2M_MAC_TRANSPORT_TEST_HOOKS)
std::atomic<bool> forceNextTeardownFallback { false };
#endif

std::uint32_t nativeCode(NSInteger code) noexcept
{
    // FetchResponse has an unsigned diagnostics field; expose the magnitude of
    // a negative NSURLError code rather than its two's-complement bit pattern.
    const auto value = static_cast<std::uint32_t>(code);
    return code < 0 ? 0U - value : value;
}

struct State
{
    explicit State(Clock::time_point end) : deadline(end)
    {
        response.body.reserve(maximumBodyBytes);
        ++activeContexts;
    }
    ~State() { --activeContexts; }

    bool stopped() const noexcept
    {
        return stop.load(std::memory_order_acquire) || Clock::now() >= deadline;
    }

    // The caller holds mutex. No allocation, native cancellation, or native
    // completion block is made by fail() while the state lock is held.
    void fail(const char* location, NSInteger error = 0) noexcept
    {
        if (completed) return;
        completed = true;
        response.success = false;
        stage = location;
        response.nativeError = nativeCode(error);
    }

    const Clock::time_point deadline;
    std::atomic<bool> stop { false };
    std::atomic<unsigned> callbacks { 0 };
    std::mutex mutex;
    std::condition_variable condition;
    FetchResponse response;
    const char* stage = "await-response";
    bool receivedResponse = false;
    bool completed = false;
    bool invalidated = false;
    unsigned redirects = 0;
};

struct CallbackScope
{
    explicit CallbackScope(State* context) : state(context)
    {
        ++activeCallbacks;
        state->callbacks.fetch_add(1, std::memory_order_acq_rel);
    }
    ~CallbackScope()
    {
        state->callbacks.fetch_sub(1, std::memory_order_acq_rel);
        --activeCallbacks;
        state->condition.notify_all();
    }
    State* const state;
};

void fail(State& state, const char* stage, NSInteger error = 0)
{
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.fail(stage, error);
    }
    state.condition.notify_all();
}

std::string utf8(NSString* value)
{
    if (value == nil) return {};
    const char* bytes = [value UTF8String];
    return bytes != nullptr ? std::string(bytes) : std::string();
}

bool allowedUrl(NSURL* url)
{
    if (url == nil || [url user] != nil || [url password] != nil) return false;
    NSString* absolute = [url absoluteString];
    if ([absolute lengthOfBytesUsingEncoding:NSUTF8StringEncoding] > 8192) return false;
    if (!isSafeHttpUrl(utf8(absolute))) return false;
    if ([[url scheme] caseInsensitiveCompare:@"https"] == NSOrderedSame) return true;
    if ([[url scheme] caseInsensitiveCompare:@"http"] != NSOrderedSame) return false;
    // Plain HTTP is an internal loopback fixture seam only. The production
    // UpdateChecker uses its fixed HTTPS source and has no endpoint preference.
    NSString* host = [url host];
    return [host isEqualToString:@"127.0.0.1"] || [host isEqualToString:@"::1"]
        || [host isEqualToString:@"[::1]"];
}

NSTimeInterval remainingSeconds(Clock::time_point deadline)
{
    return std::max(0.001, std::chrono::duration<double>(deadline - Clock::now()).count());
}

NSMutableURLRequest* anonymousRequest(NSURL* url, Clock::time_point deadline)
{
    auto* request = [[NSMutableURLRequest alloc] initWithURL:url
                                               cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                           timeoutInterval:remainingSeconds(deadline)];
    request.HTTPMethod = @"GET";
    request.HTTPShouldHandleCookies = NO;
    [request setValue:@"text/csv, text/plain;q=0.9" forHTTPHeaderField:@"Accept"];
    [request setValue:@"no-cache, no-store" forHTTPHeaderField:@"Cache-Control"];
    return request;
}

void answerChallenge(State& state, NSURLAuthenticationChallenge* challenge,
                     void (^completion)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))
{
    if (!state.stopped()
        && [[[challenge protectionSpace] authenticationMethod] isEqualToString:NSURLAuthenticationMethodServerTrust])
    {
        // Let the OS verify BOTH the certificate chain and the URL hostname.
        // Never create a credential from SecTrust or relax trust/ATS here.
        completion(NSURLSessionAuthChallengePerformDefaultHandling, nil);
        return;
    }
    fail(state, state.stopped() ? "deadline-or-cancel" : "authentication-required",
         state.stopped() ? NSURLErrorCancelled : NSURLErrorUserAuthenticationRequired);
    // Includes proxy/password/NTLM/Negotiate and client-certificate requests.
    // No credential storage, Keychain lookup, authentication UI, or client ID.
    completion(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
}

// Its address identifies THIS loaded plugin image, not Foundation or JUCE.
void imageAnchor() {}

class ImagePin
{
public:
    bool acquire()
    {
        Dl_info info {};
        if (dladdr(reinterpret_cast<const void*>(&imageAnchor), &info) == 0) return false;
        // A headless test may statically link this file into its main executable,
        // which cannot be unloaded and cannot be opened as a dylib with dlopen.
        const auto* header = static_cast<const mach_header*>(info.dli_fbase);
        if (header == nullptr) return false;
        if (header->filetype == MH_EXECUTE) return true;
        if (info.dli_fname == nullptr) return false;
        handle = dlopen(info.dli_fname, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
        if (handle != nullptr) ++temporaryImagePins;
        return handle != nullptr;
    }

    void keepLoaded() noexcept
    {
        if (handle == nullptr) return;
        void* expected = nullptr;
        if (!permanentImagePin.compare_exchange_strong(expected, handle, std::memory_order_acq_rel))
            dlclose(handle); // The already-published permanent reference protects us.
        --temporaryImagePins;
        handle = nullptr;
    }

    ~ImagePin() { if (handle != nullptr) { dlclose(handle); --temporaryImagePins; } }
    ImagePin() = default;
    ImagePin(const ImagePin&) = delete;
    ImagePin& operator=(const ImagePin&) = delete;
private:
    void* handle = nullptr;
};
} // namespace
} // namespace signal2m::update_detail::mac_transport

namespace signal2m::update_detail::mac_transport
{
namespace
{
// A distinct runtime class per request avoids Objective-C's global class-name
// collisions when several copies/versions of this plugin share a host process.
// NSObject supplies dealloc; the only extra ivar is an unretained heap pointer.
// No ARC/C++ ivars, custom dealloc, .cxx_destruct, or plugin-owned async blocks.
State** contextSlot(id object)
{
    const auto slot = class_getInstanceVariable(object_getClass(object), "lmzContext");
    auto* bytes = static_cast<char*>((__bridge void*)object);
    return reinterpret_cast<State**>(bytes + ivar_getOffset(slot));
}

State* contextOf(id object) { return *contextSlot(object); }

void didReceiveResponse(id self, SEL, NSURLSession* session, NSURLSessionDataTask* task, NSURLResponse* response, void (^completion)(NSURLSessionResponseDisposition))
{
    auto* context = contextOf(self);
    (void)session;
    (void)task;
    using namespace signal2m::update_detail;
    using namespace mac_transport;
    CallbackScope scope(context);
    auto disposition = NSURLSessionResponseCancel;
    try
    {
        if (context->stopped()) fail(*context, "deadline-or-cancel", NSURLErrorCancelled);
        else if (![response isKindOfClass:[NSHTTPURLResponse class]]) fail(*context, "non-http-response", NSURLErrorBadServerResponse);
        else
        {
            auto* http = static_cast<NSHTTPURLResponse*>(response);
            NSString* type = [http valueForHTTPHeaderField:@"Content-Type"];
            const auto declared = [response expectedContentLength];
            const auto typeBytes = [type lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
            // Convert only a bounded field; stream body chunks independently.
            const std::string contentType = typeBytes <= maximumContentTypeBytes ? utf8(type) : std::string();
            std::lock_guard<std::mutex> lock(context->mutex);
            if (!context->completed)
            {
                context->response.statusCode = static_cast<int>([http statusCode]);
                if ([http statusCode] != 200) context->fail("http-status");
                else if (typeBytes > maximumContentTypeBytes) context->fail("content-type-limit");
                else if (declared > static_cast<std::int64_t>(maximumBodyBytes)) context->fail("content-length-limit");
                else
                {
                    context->receivedResponse = true;
                    context->response.contentType = contentType;
                    context->stage = "read-body";
                    disposition = NSURLSessionResponseAllow;
                }
            }
        }
    }
    catch (...) { fail(*context, "callback-exception", NSURLErrorUnknown); }
    completion(disposition);
    context->condition.notify_all();
}

void didReceiveData(id self, SEL, NSURLSession* session, NSURLSessionDataTask* task, NSData* data)
{
    auto* context = contextOf(self);
    (void)session;
    using namespace signal2m::update_detail;
    using namespace mac_transport;
    CallbackScope scope(context);
    bool cancel = false;
    try
    {
        const auto count = static_cast<std::size_t>([data length]);
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->completed) cancel = true;
        else if (context->stopped()) { context->fail("deadline-or-cancel", NSURLErrorCancelled); cancel = true; }
        else if (!context->receivedResponse) { context->fail("body-before-response", NSURLErrorBadServerResponse); cancel = true; }
        else if (count > maximumBodyBytes - context->response.body.size()) { context->fail("body-limit"); cancel = true; }
        else if (count != 0) context->response.body.append(static_cast<const char*>([data bytes]), count);
    }
    catch (...) { fail(*context, "callback-exception", NSURLErrorUnknown); cancel = true; }
    if (cancel) [task cancel];
    context->condition.notify_all();
}

void willPerformHTTPRedirection(id self, SEL, NSURLSession* session, NSURLSessionTask* task, NSHTTPURLResponse* response, NSURLRequest* request, void (^completion)(NSURLRequest*))
{
    auto* context = contextOf(self);
    (void)session;
    (void)task;
    using namespace signal2m::update_detail::mac_transport;
    CallbackScope scope(context);
    NSURLRequest* next = nil;
    try
    {
        NSURL* target = [request URL];
        const bool downgrade = [[[[response URL] scheme] lowercaseString] isEqualToString:@"https"]
                            && ![[[target scheme] lowercaseString] isEqualToString:@"https"];
        const bool safe = !downgrade && allowedUrl(target);
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->stopped()) context->fail("deadline-or-cancel", NSURLErrorCancelled);
            else if (!safe) context->fail("unsafe-redirect", NSURLErrorRedirectToNonExistentLocation);
            else if (context->redirects >= maximumRedirects) context->fail("redirect-limit", NSURLErrorHTTPTooManyRedirects);
            else if (!context->completed) ++context->redirects;
        }
        bool proceed = false;
        { std::lock_guard<std::mutex> lock(context->mutex); proceed = !context->completed; }
        // Rebuild GET from the validated URL, discarding any cookies, auth,
        // Referer, method changes, or headers copied by native redirection.
        if (proceed) next = anonymousRequest(target, context->deadline);
    }
    catch (...) { fail(*context, "callback-exception", NSURLErrorUnknown); }
    completion(next);
    context->condition.notify_all();
}

void sessionChallenge(id self, SEL, NSURLSession* session, NSURLAuthenticationChallenge* challenge, void (^completion)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))
{
    auto* context = contextOf(self);
    (void)session;
    signal2m::update_detail::mac_transport::CallbackScope scope(context);
    signal2m::update_detail::mac_transport::answerChallenge(*context, challenge, completion);
}

void taskChallenge(id self, SEL, NSURLSession* session, NSURLSessionTask* task, NSURLAuthenticationChallenge* challenge, void (^completion)(NSURLSessionAuthChallengeDisposition, NSURLCredential*))
{
    auto* context = contextOf(self);
    (void)session;
    (void)task;
    signal2m::update_detail::mac_transport::CallbackScope scope(context);
    signal2m::update_detail::mac_transport::answerChallenge(*context, challenge, completion);
}

void didCompleteWithError(id self, SEL, NSURLSession* session, NSURLSessionTask* task, NSError* error)
{
    auto* context = contextOf(self);
    (void)session;
    (void)task;
    using namespace signal2m::update_detail::mac_transport;
    CallbackScope scope(context);
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (!context->completed)
        {
            if (context->stopped()) context->fail("deadline-or-cancel", NSURLErrorCancelled);
            else if (error != nil)
            {
                const auto code = [error code];
                context->fail("native-error", code);
                if ([[error domain] isEqualToString:NSURLErrorDomain] && code <= -1200 && code >= -1206)
                    context->response.secureFailureFlags = 1;
            }
            else if (!context->receivedResponse) context->fail("missing-response", NSURLErrorBadServerResponse);
            else { context->completed = true; context->response.success = true; context->stage = "complete"; }
        }
    }
    context->condition.notify_all();
}

void didBecomeInvalidWithError(id self, SEL, NSURLSession* session, NSError* error)
{
    auto* context = contextOf(self);
    (void)session;
    using namespace signal2m::update_detail::mac_transport;
    CallbackScope scope(context);
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->invalidated = true;
        context->fail("session-invalidated", error != nil ? [error code] : NSURLErrorCancelled);
    }
    context->condition.notify_all();
}

void willCacheResponse(id self, SEL, NSURLSession* session, NSURLSessionDataTask* task, NSCachedURLResponse* response, void (^completion)(NSCachedURLResponse*))
{
    auto* context = contextOf(self);
    (void)session;
    (void)task;
    (void)response;
    signal2m::update_detail::mac_transport::CallbackScope scope(context);
    completion(nil);
}

template<typename Function>
IMP runtimeImplementation(Function function) noexcept
{
    // The runtime stores IMP as an erased function pointer; the method encoding
    // below supplies its true signature. Avoid incompatible-function-cast
    // diagnostics while preserving the pointer representation on Darwin ABIs.
    static_assert(sizeof(Function) == sizeof(IMP));
    IMP implementation = nullptr;
    std::memcpy(&implementation, &function, sizeof(implementation));
    return implementation;
}

Class makeDelegateClass()
{
    static std::atomic<std::uint64_t> serial { 0 };
    char name[96] {};
    const auto address = reinterpret_cast<std::uintptr_t>(&imageAnchor);
    std::snprintf(name, sizeof(name), "LMZSignal2MUpdate_%llx_%llu",
                  static_cast<unsigned long long>(address),
                  static_cast<unsigned long long>(serial.fetch_add(1)));
    Class type = objc_allocateClassPair([NSObject class], name, 0);
    if (type == Nil) return Nil;
    std::uint8_t alignment = 0;
    for (auto value = alignof(State*); value > 1; value >>= 1) ++alignment;
    const bool added = class_addIvar(type, "lmzContext", sizeof(State*), alignment, "^v")
        && class_addProtocol(type, @protocol(NSURLSessionDataDelegate))
        && class_addMethod(type, @selector(URLSession:dataTask:didReceiveResponse:completionHandler:), runtimeImplementation(&didReceiveResponse), "v@:@@@@?")
        && class_addMethod(type, @selector(URLSession:dataTask:didReceiveData:), runtimeImplementation(&didReceiveData), "v@:@@@")
        && class_addMethod(type, @selector(URLSession:task:willPerformHTTPRedirection:newRequest:completionHandler:), runtimeImplementation(&willPerformHTTPRedirection), "v@:@@@@@?")
        && class_addMethod(type, @selector(URLSession:didReceiveChallenge:completionHandler:), runtimeImplementation(&sessionChallenge), "v@:@@@?")
        && class_addMethod(type, @selector(URLSession:task:didReceiveChallenge:completionHandler:), runtimeImplementation(&taskChallenge), "v@:@@@@?")
        && class_addMethod(type, @selector(URLSession:task:didCompleteWithError:), runtimeImplementation(&didCompleteWithError), "v@:@@@")
        && class_addMethod(type, @selector(URLSession:didBecomeInvalidWithError:), runtimeImplementation(&didBecomeInvalidWithError), "v@:@@")
        && class_addMethod(type, @selector(URLSession:dataTask:willCacheResponse:completionHandler:), runtimeImplementation(&willCacheResponse), "v@:@@@@?");
    if (!added) { objc_disposeClassPair(type); return Nil; }
    objc_registerClassPair(type);
    ++delegateClasses;
    return type;
}
} // namespace
} // namespace signal2m::update_detail::mac_transport


namespace signal2m::update_detail
{
FetchResponse fetchHttp(const juce::String& endpoint, RequestControl& control)
{
    using namespace mac_transport;
    FetchResponse rejected;
    rejected.stage = "validate-url";
    if (control.stopRequested())
    {
        rejected.nativeError = nativeCode(control.wasCancelled() ? NSURLErrorCancelled : NSURLErrorTimedOut);
        return rejected;
    }
    if ([NSThread isMainThread]) { rejected.stage = "background-thread-required"; return rejected; }
    if (teardownUnavailable.load(std::memory_order_acquire)) { rejected.stage = "native-teardown-unavailable"; return rejected; }
    const auto address = endpoint.toStdString();
    if (!isSafeHttpUrl(address)) { rejected.nativeError = nativeCode(NSURLErrorBadURL); return rejected; }

    ImagePin image;
    if (!image.acquire()) { rejected.stage = "pin-image"; return rejected; }
    const auto deadline = std::min(control.deadline, Clock::now() + maximumRequestTime);
    auto state = std::make_unique<State>(deadline);
    Class delegateClass = Nil;
    id delegate = nil;
    __weak id weakDelegate = nil;
    __weak NSURLSession* weakSession = nil;
    __weak NSOperationQueue* weakQueue = nil;
    bool attemptedSession = false, hadSession = false, invalidatedAndDrained = false;
    bool queueEmpty = false, networkSucceeded = false, delegateReleased = false, forcedFallback = false;
    Clock::time_point cleanupEnd;

    @autoreleasepool
    {
        NSURLSession* session = nil;
        NSURLSessionDataTask* task = nil;
        NSOperationQueue* queue = nil;
        try
        {
            @try
            {
                NSString* text = [[NSString alloc] initWithBytes:address.data() length:address.size() encoding:NSUTF8StringEncoding];
                NSURL* url = [NSURL URLWithString:text];
                if (!allowedUrl(url)) fail(*state, "validate-url", NSURLErrorBadURL);
                else if (!control.stopRequested())
                {
                    auto* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
                    configuration.URLCache = nil;
                    configuration.URLCredentialStorage = nil;
                    configuration.HTTPCookieStorage = nil;
                    configuration.HTTPShouldSetCookies = NO;
                    configuration.HTTPCookieAcceptPolicy = NSHTTPCookieAcceptPolicyNever;
                    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
                    configuration.timeoutIntervalForRequest = remainingSeconds(deadline);
                    configuration.timeoutIntervalForResource = remainingSeconds(deadline);
                    configuration.waitsForConnectivity = NO;
                    configuration.HTTPMaximumConnectionsPerHost = 1;
                    configuration.TLSMinimumSupportedProtocolVersion = tls_protocol_version_TLSv12;
                    queue = [[NSOperationQueue alloc] init];
                    queue.maxConcurrentOperationCount = 1;
                    queue.qualityOfService = NSQualityOfServiceUtility;
                    queue.name = @"sound.lmz.signal2m.update";
                    delegateClass = makeDelegateClass();
                    delegate = delegateClass != Nil ? [[delegateClass alloc] init] : nil;
                    weakDelegate = delegate;
                    if (delegate == nil) throw std::runtime_error("Cannot create native update delegate");
                    *contextSlot(delegate) = state.get();
                    attemptedSession = true;
                    session = [NSURLSession sessionWithConfiguration:configuration delegate:delegate delegateQueue:queue];
                    weakSession = session;
                    weakQueue = queue;
                    hadSession = session != nil;
                    task = [session dataTaskWithRequest:anonymousRequest(url, deadline)];
                    if (task == nil) fail(*state, "create-task", NSURLErrorUnknown);
                    else if (control.stopRequested()) fail(*state, "deadline-or-cancel", NSURLErrorCancelled);
                    else [task resume];
                    std::unique_lock<std::mutex> lock(state->mutex);
                    while (!state->completed && !control.stopRequested() && Clock::now() < deadline)
                        state->condition.wait_until(lock, std::min(deadline, Clock::now() + cancellationPoll));
                    if (control.stopRequested() || Clock::now() >= deadline)
                    {
                        state->completed = true;
                        state->response.success = false;
                        state->stage = "deadline-or-cancel";
                        state->response.nativeError = nativeCode(control.wasCancelled() ? NSURLErrorCancelled : NSURLErrorTimedOut);
                    }
                }
                else fail(*state, "deadline-or-cancel", NSURLErrorCancelled);
            }
            @catch (NSException* exception) { (void)exception; fail(*state, "objective-c-exception", NSURLErrorUnknown); }
        }
        catch (...) { fail(*state, "transport-exception", NSURLErrorUnknown); }

        // No request, lock, stack reference, or RequestControl enters a callback.
        // Cancel is asynchronous, so retain our code and heap context until the
        // invalidation callback has returned AND its serial queue has drained.
        state->stop.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            networkSucceeded = state->response.success;
            lastNetworkStage.store(state->stage, std::memory_order_release);
        }
        cleanupEnd = Clock::now() + cleanupBudget;
        @try
        {
            [session invalidateAndCancel];
            while (hadSession && Clock::now() < cleanupEnd)
            {
                bool invalidated = false;
                { std::lock_guard<std::mutex> lock(state->mutex); invalidated = state->invalidated; }
                queueEmpty = [queue operationCount] == 0;
                if (invalidated && state->callbacks.load(std::memory_order_acquire) == 0 && queueEmpty)
                { invalidatedAndDrained = true; break; }
                std::this_thread::sleep_for(1ms);
            }
        }
        @catch (NSException* exception) { (void)exception; invalidatedAndDrained = false; }
        task = nil;
        session = nil;
        queue = nil;
    } // Drain task/session/queue autoreleases while our delegate is still owned.

    // Invalidation breaks native references to our delegate/callback objects;
    // it does not promise deallocation of NSURLSession or NSOperationQueue.
    // Foundation may retain those system objects after our callbacks are gone.
    // Observe them for diagnostics, and prove unload safety with the terminal
    // invalidation callback, drained serial queue, and destroyed delegate.
    // https://developer.apple.com/documentation/foundation/urlsession/invalidateandcancel()
    bool sessionGone = false, queueGone = false;
    @autoreleasepool { sessionGone = weakSession == nil; queueGone = weakQueue == nil; }
    bool quiescent = !attemptedSession || (hadSession && invalidatedAndDrained
                            && state->callbacks.load(std::memory_order_acquire) == 0);
#if defined(SIGNAL2M_MAC_TRANSPORT_TEST_HOOKS)
    // Fault injection belongs exclusively to the independent test dylib. It
    // takes the real pin/quarantine/circuit-breaker path, and cannot reset it.
    forcedFallback = forceNextTeardownFallback.exchange(false, std::memory_order_acq_rel);
    if (forcedFallback) quiescent = false;
#endif
    if (quiescent)
    {
        // Invalidation is terminal and all queued callbacks have returned.
        // Keep the heap context valid through the delegate's final release;
        // free it only after no strong native callback holder owns the object.
        delegate = nil;
        delegateReleased = true;
        for (;;)
        {
            @autoreleasepool { quiescent = weakDelegate == nil; }
            if (quiescent || Clock::now() >= cleanupEnd) break;
            std::this_thread::sleep_for(1ms);
        }
        if (quiescent && delegateClass != Nil) { objc_disposeClassPair(delegateClass); --delegateClasses; }
    }
    if (!quiescent)
    {
        // Never trade bounded destruction for a callback into an unloaded
        // image. Only a single permanent dlopen reference is published even
        // if several plugin instances discover a stuck teardown concurrently.
        image.keepLoaded();
        teardownUnavailable.store(true, std::memory_order_release);
        ++teardownFallbacks;
        { std::lock_guard<std::mutex> lock(state->mutex); state->response.success = false; state->stage = "native-teardown-timeout"; }
        delegate = nil;
    }

    std::uint32_t flags = (attemptedSession ? 1U : 0U) | (hadSession ? 2U : 0U)
                        | (queueEmpty ? 8U : 0U) | (sessionGone ? 32U : 0U)
                        | (queueGone ? 64U : 0U) | (networkSucceeded ? 256U : 0U)
                        | (quiescent ? 512U : 0U) | (forcedFallback ? 1024U : 0U)
                        | (delegateReleased ? 2048U : 0U);
    { std::lock_guard<std::mutex> lock(state->mutex); if (state->invalidated) flags |= 4U; }
    if (state->callbacks.load(std::memory_order_acquire) == 0) flags |= 16U;
    @autoreleasepool { if (weakDelegate == nil) flags |= 128U; }
    lastTeardownFlags.store(flags, std::memory_order_release);

    // Quarantine BEFORE any result-string allocation that could throw. This
    // context must remain alive even if copying diagnostics fails under OOM.
    State* resultState = state.get();
    if (!quiescent) (void)state.release();
    FetchResponse result;
    {
        std::lock_guard<std::mutex> lock(resultState->mutex);
        result = std::move(resultState->response);
        result.stage = resultState->stage;
    }
    return result;
}

// Read-only headless diagnostics. No reset/unpin seam exists in production.
std::uint64_t macUpdateActiveContexts() noexcept { return mac_transport::activeContexts.load(); }
std::uint64_t macUpdateActiveCallbacks() noexcept { return mac_transport::activeCallbacks.load(); }
std::uint64_t macUpdateTeardownFallbacks() noexcept { return mac_transport::teardownFallbacks.load(); }
bool macUpdateImagePermanentlyPinned() noexcept { return mac_transport::permanentImagePin.load() != nullptr; }
std::uint64_t macUpdateTemporaryImagePins() noexcept { return mac_transport::temporaryImagePins.load(); }
std::uint64_t macUpdateDelegateClasses() noexcept { return mac_transport::delegateClasses.load(); }
std::uint32_t macUpdateLastTeardownFlags() noexcept { return mac_transport::lastTeardownFlags.load(std::memory_order_acquire); }
const char* macUpdateLastNetworkStage() noexcept { return mac_transport::lastNetworkStage.load(std::memory_order_acquire); }
#if defined(SIGNAL2M_MAC_TRANSPORT_TEST_HOOKS)
void macUpdateForceNextTeardownFallbackForTest() noexcept { mac_transport::forceNextTeardownFallback.store(true, std::memory_order_release); }
#endif
} // namespace signal2m::update_detail
#endif
