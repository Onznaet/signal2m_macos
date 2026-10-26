#include "UpdateChecker.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace signal2m::update_detail
{
namespace
{
struct AsyncEvents
{
    explicit AsyncEvents(RequestControl& source) : control(source) {}
    RequestControl& control;
    DWORD completed = 0;
    DWORD readBytes = 0;
    DWORD nativeError = 0;
    DWORD secureFailureFlags = 0;
    bool failed = false;
    std::array<char, 8192> buffer {};
};

void CALLBACK statusCallback(HINTERNET, DWORD_PTR context, DWORD status, void* information, DWORD informationBytes)
{
    if (context == 0) return;
    auto& events = *reinterpret_cast<AsyncEvents*>(context);
    {
        std::lock_guard<std::mutex> lock(events.control.mutex);
        if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
        {
            events.failed = true;
            if (information != nullptr && informationBytes >= sizeof(WINHTTP_ASYNC_RESULT))
                events.nativeError = static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError;
        }
        else if (status == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE)
        {
            if (information != nullptr && informationBytes >= sizeof(DWORD))
                events.secureFailureFlags = *static_cast<DWORD*>(information);
        }
        else if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE)
        {
            events.readBytes = informationBytes;
            events.completed |= status;
        }
        else if (status == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE
                 || status == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)
            events.completed |= status;
    }
    events.control.condition.notify_all();
}

bool waitFor(AsyncEvents& events, DWORD completion)
{
    auto& control = events.control;
    std::unique_lock<std::mutex> lock(control.mutex);
    control.condition.wait_until(lock, control.deadline, [&]
    {
        return (events.completed & completion) != 0 || events.failed || control.wasCancelled();
    });
    if (control.stopRequested() || events.failed || (events.completed & completion) == 0)
        return false;
    events.completed &= ~completion;
    return true;
}

struct Handles
{
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    HANDLE drained = nullptr;
    bool drainRegistered = false;

    ~Handles()
    {
        // Only this worker closes the handles, outside the callback mutex. Close
        // cancels pending asynchronous DNS/connect/read, including a slow body.
        if (request != nullptr) WinHttpCloseHandle(request);
        if (connection != nullptr) WinHttpCloseHandle(connection);
        if (session != nullptr) WinHttpCloseHandle(session);
        // Microsoft documents this session event as firing after the final
        // callback has completed, stronger than merely observing HANDLE_CLOSING.
        // Keep both the event and callback context alive through native teardown.
        // No plugin callback remains executable after the worker is joined.
        // https://github.com/MicrosoftDocs/win32/blob/docs/desktop-src/WinHttp/option-flags.md
        if (drainRegistered) WaitForSingleObject(drained, INFINITE);
        if (drained != nullptr) CloseHandle(drained);
    }
};

bool setDword(HINTERNET handle, DWORD option, DWORD value)
{
    return WinHttpSetOption(handle, option, &value, sizeof(value)) != FALSE;
}

std::string contentType(HINTERNET request)
{
    std::array<wchar_t, 2048> value {};
    DWORD bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                            value.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
        return {};
    return juce::String(value.data()).toStdString();
}
} // namespace

FetchResponse fetchHttp(const juce::String& endpoint, RequestControl& control)
{
    FetchResponse response;
    response.stage = "validate-url";
    if (control.stopRequested())
    {
        response.nativeError = control.wasCancelled() ? ERROR_WINHTTP_OPERATION_CANCELLED : ERROR_WINHTTP_TIMEOUT;
        return response;
    }
    if (!isSafeHttpUrl(endpoint.toStdString()))
    {
        response.nativeError = ERROR_WINHTTP_INVALID_URL;
        return response;
    }
    const std::wstring address(endpoint.toWideCharPointer());
    URL_COMPONENTS components {};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = components.dwUrlPathLength = components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(address.c_str(), static_cast<DWORD>(address.size()), 0, &components)
        || (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS))
    {
        response.nativeError = GetLastError();
        response.stage = "crack-url";
        return response;
    }
    const std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path;
    if (components.dwUrlPathLength > 0) path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (path.empty()) path = L"/";
    if (components.dwExtraInfoLength > 0) path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (const auto fragment = path.find(L'#'); fragment != std::wstring::npos) path.erase(fragment);

    // Declaration order matters: Handles drains every native callback before
    // AsyncEvents and its read buffer leave scope on every return/exception path.
    AsyncEvents events(control);
    const auto fail = [&](const char* stage)
    {
        const DWORD immediateError = GetLastError();
        response.stage = stage;
        {
            std::lock_guard<std::mutex> lock(control.mutex);
            response.nativeError = events.nativeError;
            response.secureFailureFlags = events.secureFailureFlags;
        }
        if (response.nativeError == 0)
            response.nativeError = control.wasCancelled() ? ERROR_WINHTTP_OPERATION_CANCELLED
                                  : control.stopRequested() ? ERROR_WINHTTP_TIMEOUT : immediateError;
        return response;
    };
    Handles handles;
    handles.drained = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (handles.drained == nullptr) return fail("create-drain-event");
    handles.session = WinHttpOpen(L"Signal2M UpdateChecker", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (handles.session == nullptr) return fail("open-session");
    if (!WinHttpSetOption(handles.session, WINHTTP_OPTION_UNLOAD_NOTIFY_EVENT,
                          &handles.drained, sizeof(handles.drained)))
        return fail("register-unload-event"); // Fail before registering a callback on unsupported systems.
    handles.drainRegistered = true;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(control.deadline - RequestControl::Clock::now());
    const int milliseconds = static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, maximumRequestTime.count()));
    if (!WinHttpSetTimeouts(handles.session, milliseconds, milliseconds, milliseconds, milliseconds)
        || !setDword(handles.session, WINHTTP_OPTION_SECURE_PROTOCOLS, WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2))
        return fail("configure-session");
    handles.connection = WinHttpConnect(handles.session, host.c_str(), components.nPort, 0);
    if (handles.connection == nullptr || control.stopRequested()) return fail("connect");
    const DWORD flags = WINHTTP_FLAG_REFRESH | (components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0U);
    handles.request = WinHttpOpenRequest(handles.connection, L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (handles.request == nullptr) return fail("open-request");
    if (!setDword(handles.request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, 3)
        || !setDword(handles.request, WINHTTP_OPTION_REDIRECT_POLICY, WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP)
        || !setDword(handles.request, WINHTTP_OPTION_DISABLE_FEATURE, WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION)
        || !setDword(handles.request, WINHTTP_OPTION_AUTOLOGON_POLICY, WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH)
        || !setDword(handles.request, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, 16U * 1024U))
        return fail("configure-request");
    // Public anonymous CSV: never offer a user's client certificate. This does
    // not change server certificate/hostname verification or ignore TLS errors.
    // https://learn.microsoft.com/en-us/windows/win32/winhttp/option-flags
    if (components.nScheme == INTERNET_SCHEME_HTTPS
        && !WinHttpSetOption(handles.request, WINHTTP_OPTION_CLIENT_CERT_CONTEXT, WINHTTP_NO_CLIENT_CERT_CONTEXT, 0))
        return fail("anonymous-client-certificate");
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(&events);
    if (!WinHttpSetOption(handles.request, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)))
        return fail("set-context");
    if (WinHttpSetStatusCallback(handles.request, statusCallback,
                                  WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES | WINHTTP_CALLBACK_FLAG_SECURE_FAILURE, 0)
        == WINHTTP_INVALID_STATUS_CALLBACK)
        return fail("set-callback");
    if (control.stopRequested()) return fail("before-send");
    if (!WinHttpSendRequest(handles.request, L"Accept: text/csv, text/plain;q=0.9\r\nCache-Control: no-cache\r\n",
                            static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, context)
        || !waitFor(events, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE))
        return fail("send");
    if (!WinHttpReceiveResponse(handles.request, nullptr) || !waitFor(events, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE))
        return fail("receive-headers");

    DWORD status = 0, bytes = sizeof(status);
    if (!WinHttpQueryHeaders(handles.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
        return fail("query-status");
    response.statusCode = static_cast<int>(status);
    if (status != 200) { response.stage = "http-status"; return response; }
    response.contentType = contentType(handles.request);
    DWORD declaredLength = 0;
    bytes = sizeof(declaredLength);
    if (WinHttpQueryHeaders(handles.request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &declaredLength, &bytes, WINHTTP_NO_HEADER_INDEX)
        && declaredLength > maximumBodyBytes)
    {
        response.stage = "content-length-limit";
        return response;
    }
    response.body.reserve(std::min<std::size_t>(declaredLength, maximumBodyBytes));
    while (!control.stopRequested())
    {
        // One extra byte is sufficient to reject a response over the cap. No
        // server-controlled allocation and no recursive read from a callback.
        const auto amount = std::min(events.buffer.size(), maximumBodyBytes - response.body.size() + 1U);
        if (!WinHttpReadData(handles.request, events.buffer.data(), static_cast<DWORD>(amount), nullptr)
            || !waitFor(events, WINHTTP_CALLBACK_STATUS_READ_COMPLETE))
            return fail("read-body");
        DWORD received = 0;
        {
            std::lock_guard<std::mutex> lock(control.mutex);
            received = events.readBytes;
        }
        if (received == 0)
        {
            response.success = !control.stopRequested();
            response.stage = "complete";
            return response;
        }
        if (received > maximumBodyBytes - response.body.size())
        {
            response.stage = "body-limit";
            return response;
        }
        response.body.append(events.buffer.data(), received);
    }
    return fail("deadline-or-cancel");
}
} // namespace signal2m::update_detail
#else
namespace signal2m::update_detail
{
FetchResponse fetchHttp(const juce::String&, RequestControl&) { return {}; }
} // namespace signal2m::update_detail
#endif
