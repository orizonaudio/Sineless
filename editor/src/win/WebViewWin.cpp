#include "EmbeddedAssets.h"
#include "editor/WebView.h"

// windows.h first: the other Windows headers depend on it. Not the lean variant: WebView2.h needs
// its COM declarations.
// clang-format off
#define NOMINMAX
#include <windows.h>
// clang-format on
#include "WebViewWinTesting.h"
#include "WinModule.h"

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <chrono>
#include <cwchar>
#include <deque>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <utility>
#include <vector>
#include <wrl.h>

// The Windows counterpart of WebViewMac.mm, on WebView2. The page loads from orizon://ui, a
// custom scheme answered from the UI embedded in the binary, and fetches each display frame from
// orizon://ui/frame, a request held until a frame is ready. WebView2 starts asynchronously: until
// it is ready, messages for the page wait in a queue.
//
// WebView2 sits in a child window of ours inside the host's window. It paints the page's
// background colour from the start, and tells WebView2 when the editor is hidden, which
// WebView2 cannot see by itself (the page would keep drawing). The page is told the display
// scale instead of following the monitor's, so that it always lays out in the editor's CSS size.

namespace orizon::editor {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kStartUrl = L"orizon://ui/index.html";
// The page stays on the embedded UI: navigations elsewhere (a link, a file dropped on the
// editor) are cancelled, and messages from anywhere else are ignored.
constexpr std::wstring_view kUiOrigin = L"orizon://ui/";
// A renderer that keeps crashing is not reloaded forever: at most this many times a minute.
constexpr std::size_t kMaxReloadsPerMinute = 3;
constexpr const wchar_t* kFrameUrl = L"orizon://ui/frame";
// The key script's reports share the page's channel; control messages are JSON objects.
constexpr std::wstring_view kFocusPrefix = L"orizonFocus:";
constexpr std::wstring_view kKeyPrefix = L"orizonKey:";

// Keystrokes go to the host unless the user is typing into the page. WebView2 runs in another
// process, so the host never sees the page's keys: shortcuts arrive as AcceleratorKeyPressed
// and are forwarded from there; other keys reach only the page, which reports them
// ("orizonKey:" + d(own) or u(p), r if repeated, ":" + virtual key) instead of handling them.
constexpr const wchar_t* kKeyScript = LR"JS(
(() => {
  const nonText = new Set(["button", "checkbox", "color", "file", "hidden", "image", "radio",
                           "range", "reset", "submit"]);
  const isEditable = (el) => !!el && (el.isContentEditable || el.tagName === "TEXTAREA" ||
      el.tagName === "SELECT" || (el.tagName === "INPUT" && !nonText.has(el.type)));
  const report = () => window.chrome.webview.postMessage(
      "orizonFocus:" + (isEditable(document.activeElement) ? "1" : "0"));
  document.addEventListener("focusin", report, true);
  document.addEventListener("focusout", () => setTimeout(report, 0), true);
  const forward = (event) => {
    if (isEditable(document.activeElement)) return;
    event.preventDefault();
    event.stopImmediatePropagation();
    window.chrome.webview.postMessage("orizonKey:" + (event.type === "keydown" ? "d" : "u") +
        (event.repeat ? "r" : "") + ":" + event.keyCode);
  };
  window.addEventListener("keydown", forward, true);
  window.addEventListener("keyup", forward, true);
})();
)JS";

// The page's background (--panel), shown until the page draws.
constexpr COREWEBVIEW2_COLOR kBackground{255, 0x16, 0x1b, 0x21};
constexpr wchar_t kContainerClass[] = L"OrizonOScopeWebViewHost";
constexpr UINT_PTR kVisibilityTimer = 1;
constexpr UINT kVisibilityInterval = 250; // ms

void paintFailure(HWND window);

// Hosts open DPI-unaware plugin windows to have Windows stretch them (Live's Auto-Scale, for
// example). WebView2 cannot put its window under an unaware one at most display scales
// (ERROR_INVALID_STATE: Windows forbids parenting across DPI awareness), so there it uses
// Window-to-Visual hosting, drawing into a visual of the window instead: it works at every
// scale, takes input as usual, and draws sharp. WebView2 reads the mode from an environment
// variable while it creates the environment; the variable is set for that call only, since
// the host may run other WebView2s. Unaware windows have a profile, and so a browser process,
// of their own, so the two modes never share one.
constexpr wchar_t kHostingModeVariable[] = L"COREWEBVIEW2_FORCED_HOSTING_MODE";
constexpr wchar_t kWindowToVisual[] = L"COREWEBVIEW2_HOSTING_MODE_WINDOW_TO_VISUAL";

bool isDpiUnaware(HWND window)
{
    return GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(window)) ==
           DPI_AWARENESS_UNAWARE;
}

// Sets one of this process's environment variables until the end of the scope.
class ScopedEnvironmentVariable
{
public:
    ScopedEnvironmentVariable(const wchar_t* variableName, const wchar_t* value)
        : name(variableName)
    {
        const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
        if (length > 0) {
            previous.resize(length);
            GetEnvironmentVariableW(name, previous.data(), length);
            previous.resize(length - 1); // Without the terminator.
            hadValue = true;
        }
        SetEnvironmentVariableW(name, value);
    }
    ~ScopedEnvironmentVariable()
    {
        SetEnvironmentVariableW(name, hadValue ? previous.c_str() : nullptr);
    }
    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
    const wchar_t* name;
    std::wstring previous;
    bool hadValue = false;
};

// Runs the enclosed calls in `window`'s DPI awareness, so that the windows they create match
// it: a host can open a DPI-unaware plugin window (stretched by Windows) from an aware thread.
class DpiContext
{
public:
    explicit DpiContext(HWND window)
        : previous(SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(window)))
    {}
    ~DpiContext()
    {
        if (previous != nullptr) {
            SetThreadDpiAwarenessContext(previous);
        }
    }
    DpiContext(const DpiContext&) = delete;
    DpiContext& operator=(const DpiContext&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous;
};

std::wstring widen(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string narrow(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size,
                        nullptr, nullptr);
    return utf8;
}

// WebView2 keeps its profile in a user-data folder. The default sits next to the host's
// executable, which may not be writable, so it is set explicitly:
// %LOCALAPPDATA%\Orizon Audio\O-Scope\WebView2-<kProfileVersion>.
// Processes share a profile's browser process, and WebView2 refuses (ERROR_INVALID_STATE) one
// whose environment options differ: two hosts running O-Scope versions with different options
// would clash. kProfileVersion changes with the options, so that each such version has its own.
// WebView2 refuses (ERROR_INVALID_STATE) to add a view to a browser process whose views have
// another DPI awareness, and editors share a browser process through their profile folder:
// DPI-unaware host windows (Live's Auto-Scale, for example) get a folder of their own.
std::wstring& browserFolderForTests()
{
    static std::wstring folder;
    return folder;
}

// Increase whenever the environment options (scheme registration, arguments) change.
constexpr int kProfileVersion = 1;

std::wstring userDataFolder(HWND parent)
{
    PWSTR localAppData = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        folder = std::wstring(localAppData) + L"\\Orizon Audio\\O-Scope\\WebView2-" +
                 std::to_wstring(kProfileVersion);
        switch (GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(parent))) {
        case DPI_AWARENESS_UNAWARE:
            folder += L"-Unaware";
            break;
        case DPI_AWARENESS_SYSTEM_AWARE:
            folder += L"-SystemAware";
            break;
        default:
            break;
        }
    }
    CoTaskMemFree(localAppData);
    return folder;
}

class WebViewWin final : public WebView
{
public:
    WebViewWin(HWND parentWindow, int initialWidth, int initialHeight, MessageHandler messages,
               FrameRequestHandler frameRequests);
    ~WebViewWin() override;
    WebViewWin(const WebViewWin&) = delete;
    WebViewWin& operator=(const WebViewWin&) = delete;

    void setSize(int width, int height) override;
    void setScale(double scale) override;
    void postMessage(std::string_view message) override;
    void sendFrame(std::span<const std::byte> frame) override;
    [[nodiscard]] void* nativeView() const override { return parent; }
    void evaluate(std::string_view script, std::function<void(std::string)> done) override;
    bool terminateProcessForTests(bool browser);

private:
    // Lets WebView2's completion handlers, which may run after this object is gone, find it.
    struct Link
    {
        WebViewWin* owner = nullptr;
    };

    static LRESULT CALLBACK containerProcedure(HWND handle, UINT message, WPARAM wParam,
                                               LPARAM lParam);
    void updateVisibility();
    // Shows why WebView2 could not start, painted by the container instead of the page.
    void showFailure(HRESULT result);
    void showMessage(const wchar_t* text);
    // A crashed or hung renderer is reloaded; when the browser process exits, every view of it
    // is closed, and the editor has to be reopened.
    void processFailed(ICoreWebView2ProcessFailedEventArgs* args);
    void environmentCreated(ICoreWebView2Environment* created);
    void controllerCreated(ICoreWebView2Controller* created);
    void resourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args);
    void messageReceived(ICoreWebView2WebMessageReceivedEventArgs* args);
    void acceleratorKeyPressed(ICoreWebView2AcceleratorKeyPressedEventArgs* args);
    void forwardPageKey(std::wstring_view report);
    void requestFrame(ComPtr<ICoreWebView2WebResourceRequestedEventArgs> args);
    void answer(ICoreWebView2WebResourceRequestedEventArgs* args, const void* data,
                std::size_t size, const wchar_t* contentType, int status = 200);
    void answerFrame(const std::vector<std::byte>& frame);

    HWND parent = nullptr;
    HWND container = nullptr;
    int width = 0;
    int height = 0;
    double scale = 1.0;
    bool isShown = true;
    MessageHandler onMessage;
    FrameRequestHandler onFrameRequest;
    std::shared_ptr<Link> link = std::make_shared<Link>();
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> core;
    EventRegistrationToken resourceToken{};
    EventRegistrationToken messageToken{};
    EventRegistrationToken acceleratorToken{};
    EventRegistrationToken navigationToken{};
    EventRegistrationToken newWindowToken{};
    EventRegistrationToken processFailedToken{};
    std::deque<std::chrono::steady_clock::time_point> reloads; // In the last minute.
    bool isEditableFocused = false;
    std::deque<std::wstring> queuedMessages; // Until WebView2 is ready.
    // The page's open frame request, and a frame produced while none was open.
    ComPtr<ICoreWebView2WebResourceRequestedEventArgs> frameRequest;
    ComPtr<ICoreWebView2Deferral> frameDeferral;
    std::vector<std::byte> heldFrame;
    bool hasHeldFrame = false;
};

WebViewWin::WebViewWin(HWND parentWindow, int initialWidth, int initialHeight,
                       MessageHandler messages, FrameRequestHandler frameRequests)
    : parent(parentWindow), width(initialWidth), height(initialHeight),
      onMessage(std::move(messages)), onFrameRequest(std::move(frameRequests))
{
    link->owner = this;
    pinThisModule();
    {
        static const HBRUSH background =
            CreateSolidBrush(RGB(kBackground.R, kBackground.G, kBackground.B));
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &WebViewWin::containerProcedure;
        windowClass.hInstance = thisModule();
        windowClass.hbrBackground = background;
        windowClass.lpszClassName = kContainerClass;
        RegisterClassExW(&windowClass); // Fails harmlessly once the class exists.
        const DpiContext dpi(parent);
        container =
            CreateWindowExW(0, kContainerClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0,
                            width, height, parent, nullptr, windowClass.hInstance, nullptr);
        SetWindowLongPtrW(container, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetTimer(container, kVisibilityTimer, kVisibilityInterval, nullptr);
    }
    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    ComPtr<ICoreWebView2EnvironmentOptions4> schemeOptions;
    if (SUCCEEDED(options.As(&schemeOptions))) {
        // orizon:// behaves like a secure origin of its own, as WKWebView's scheme handler does.
        const wchar_t* allowedOrigins[] = {L"orizon://ui"};
        auto scheme = Microsoft::WRL::Make<CoreWebView2CustomSchemeRegistration>(L"orizon");
        scheme->put_TreatAsSecure(TRUE);
        scheme->put_HasAuthorityComponent(TRUE);
        scheme->SetAllowedOrigins(1, allowedOrigins);
        ICoreWebView2CustomSchemeRegistration* registrations[] = {scheme.Get()};
        schemeOptions->SetCustomSchemeRegistrations(1, registrations);
    }
    const std::wstring folder = userDataFolder(parent);
    std::optional<ScopedEnvironmentVariable> hostingMode;
    if (isDpiUnaware(parent)) {
        hostingMode.emplace(kHostingModeVariable, kWindowToVisual);
    }
    const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        browserFolderForTests().empty() ? nullptr : browserFolderForTests().c_str(),
        folder.empty() ? nullptr : folder.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [link = link](HRESULT result, ICoreWebView2Environment* created) -> HRESULT {
                if (link->owner == nullptr) {
                    return S_OK;
                }
                if (SUCCEEDED(result)) {
                    link->owner->environmentCreated(created);
                } else {
                    link->owner->showFailure(result);
                }
                return S_OK;
            })
            .Get());
    hostingMode.reset();
    if (FAILED(started)) {
        showFailure(started); // No WebView2 Runtime installed, for example.
    }
}

WebViewWin::~WebViewWin()
{
    link->owner = nullptr;
    if (frameDeferral) {
        frameDeferral->Complete(); // Never leave the page's request hanging.
    }
    if (core) {
        core->remove_WebResourceRequested(resourceToken);
        core->remove_WebMessageReceived(messageToken);
        core->remove_NavigationStarting(navigationToken);
        core->remove_NewWindowRequested(newWindowToken);
        core->remove_ProcessFailed(processFailedToken);
    }
    if (controller) {
        controller->remove_AcceleratorKeyPressed(acceleratorToken);
        // Hidden first: WebView2 leaves a handle to an exited process of its own in ours for each
        // view closed while visible.
        controller->put_IsVisible(FALSE);
        controller->Close();
    }
    SetWindowLongPtrW(container, GWLP_USERDATA, 0);
    DestroyWindow(container); // Also stops its timer.
}

LRESULT CALLBACK WebViewWin::containerProcedure(HWND handle, UINT message, WPARAM wParam,
                                                LPARAM lParam)
{
    auto* self = reinterpret_cast<WebViewWin*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (self != nullptr && message == WM_TIMER && wParam == kVisibilityTimer) {
        self->updateVisibility();
        return 0;
    }
    if (message == WM_PAINT && GetWindowTextLengthW(handle) > 0) {
        paintFailure(handle);
        return 0;
    }
    return DefWindowProcW(handle, message, wParam, lParam);
}

void WebViewWin::showFailure(HRESULT result)
{
    wchar_t text[256];
    if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        std::swprintf(text, std::size(text),
                      L"O-Scope needs the Microsoft Edge WebView2 Runtime.\n"
                      L"Install it from developer.microsoft.com/microsoft-edge/webview2/consumer,\n"
                      L"then reopen the editor.");
    } else {
        std::swprintf(text, std::size(text),
                      L"O-Scope's editor could not start (WebView2 error 0x%08lX).\n"
                      L"Close and reopen it; if that does not help, restart the host.",
                      static_cast<unsigned long>(result));
    }
    showMessage(text);
}

void WebViewWin::showMessage(const wchar_t* text)
{
    SetWindowTextW(container, text);
    InvalidateRect(container, nullptr, TRUE);
}

void WebViewWin::processFailed(ICoreWebView2ProcessFailedEventArgs* args)
{
    COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
    args->get_ProcessFailedKind(&kind);
    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
        controller->put_IsVisible(FALSE); // Shows the message underneath.
        showMessage(L"O-Scope's editor stopped: its WebView2 browser exited.\n"
                    L"Close and reopen the editor.");
        return;
    }
    if (kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED &&
        kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
        return; // GPU and utility processes, and subframes, recover by themselves.
    }
    const auto now = std::chrono::steady_clock::now();
    while (!reloads.empty() && now - reloads.front() > std::chrono::minutes(1)) {
        reloads.pop_front();
    }
    if (reloads.size() >= kMaxReloadsPerMinute) {
        controller->put_IsVisible(FALSE);
        showMessage(L"O-Scope's editor keeps failing.\nClose and reopen it; if that does not "
                    L"help, restart the host.");
        return;
    }
    reloads.push_back(now);
    if (frameDeferral) {
        frameDeferral->Complete(); // The page that asked is gone.
        frameRequest.Reset();
        frameDeferral.Reset();
    }
    isEditableFocused = false;
    // Loaded afresh, not reloaded: a page that never finished loading has nothing to reload. The
    // new page says hello and gets the session's state again.
    core->Navigate(kStartUrl);
}

// The container's text, centred in the system's message font at the window's DPI.
void paintFailure(HWND window)
{
    std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    text.resize(static_cast<std::size_t>(
        GetWindowTextW(window, text.data(), static_cast<int>(text.size()))));
    PAINTSTRUCT paint;
    HDC context = BeginPaint(window, &paint);
    RECT bounds;
    GetClientRect(window, &bounds);
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                               GetDpiForWindow(window));
    HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
    HGDIOBJ previousFont = SelectObject(context, font);
    SetTextColor(context, RGB(0xe6, 0xe2, 0xd8)); // --ink
    SetBkMode(context, TRANSPARENT);
    RECT measured = bounds;
    DrawTextW(context, text.c_str(), -1, &measured, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    RECT centred = bounds;
    centred.top = (bounds.bottom - (measured.bottom - measured.top)) / 2;
    DrawTextW(context, text.c_str(), -1, &centred, DT_CENTER | DT_WORDBREAK);
    SelectObject(context, previousFont);
    DeleteObject(font);
    EndPaint(window, &paint);
}

// Hidden covers a closed or minimised host window too: hosts hide their plugin windows then.
void WebViewWin::updateVisibility()
{
    const bool shown =
        IsWindowVisible(container) != FALSE && IsIconic(GetAncestor(parent, GA_ROOT)) == FALSE;
    if (shown == isShown || !controller) {
        return;
    }
    isShown = shown;
    controller->put_IsVisible(shown ? TRUE : FALSE); // Hidden pages pause requestAnimationFrame.
}

void WebViewWin::environmentCreated(ICoreWebView2Environment* created)
{
    environment = created;
    // WebView2 can leave the thread in another DPI awareness by the time it completes; the
    // host's own is restored first thing then.
    const DPI_AWARENESS_CONTEXT hostAwareness = GetThreadDpiAwarenessContext();
    const DpiContext dpi(parent);
    const auto completed = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [link = link, hostAwareness](HRESULT result, ICoreWebView2Controller* made) -> HRESULT {
            SetThreadDpiAwarenessContext(hostAwareness);
            if (SUCCEEDED(result) && link->owner != nullptr) {
                link->owner->controllerCreated(made);
            } else if (SUCCEEDED(result) && made != nullptr) {
                made->put_IsVisible(FALSE); // See ~WebViewWin.
                made->Close();              // The editor closed while WebView2 started.
            } else if (FAILED(result) && link->owner != nullptr) {
                link->owner->showFailure(result);
            }
            return S_OK;
        });
    // The page's background colour from the start: set only once the controller exists, it can
    // still leave a white flicker first (a known WebView2 issue). Older runtimes lack these
    // options; controllerCreated sets the colour then.
    ComPtr<ICoreWebView2Environment10> withOptions;
    ComPtr<ICoreWebView2ControllerOptions> options;
    ComPtr<ICoreWebView2ControllerOptions3> colour;
    if (SUCCEEDED(environment.As(&withOptions)) &&
        SUCCEEDED(withOptions->CreateCoreWebView2ControllerOptions(&options)) &&
        SUCCEEDED(options.As(&colour)) &&
        SUCCEEDED(colour->put_DefaultBackgroundColor(kBackground))) {
        withOptions->CreateCoreWebView2ControllerWithOptions(container, options.Get(),
                                                             completed.Get());
    } else {
        environment->CreateCoreWebView2Controller(container, completed.Get());
    }
}

void WebViewWin::controllerCreated(ICoreWebView2Controller* created)
{
    const DpiContext dpi(parent);
    controller = created;
    controller->get_CoreWebView2(&core);
    ComPtr<ICoreWebView2Controller2> background;
    if (SUCCEEDED(controller.As(&background))) {
        background->put_DefaultBackgroundColor(kBackground);
    }
    ComPtr<ICoreWebView2Controller3> scaling;
    if (SUCCEEDED(controller.As(&scaling))) {
        scaling->put_ShouldDetectMonitorScaleChanges(FALSE);
    }
    ComPtr<ICoreWebView2Controller4> drops;
    if (SUCCEEDED(controller.As(&drops))) {
        drops->put_AllowExternalDrop(FALSE); // A file dropped on the editor would replace the UI.
    }
    setScale(scale);
    setSize(width, height);
    isShown = true;
    updateVisibility();

    ComPtr<ICoreWebView2Settings> settings;
    core->get_Settings(&settings);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE); // Ctrl + scroll zooms the trace, not the page.
    ComPtr<ICoreWebView2Settings4> autofill;
    if (SUCCEEDED(settings.As(&autofill))) {
        autofill->put_IsGeneralAutofillEnabled(FALSE); // The value fields are plain inputs.
        autofill->put_IsPasswordAutosaveEnabled(FALSE);
    }
    ComPtr<ICoreWebView2Settings5> pinch;
    if (SUCCEEDED(settings.As(&pinch))) {
        pinch->put_IsPinchZoomEnabled(FALSE); // A touch pinch would scale the whole page.
    }
    ComPtr<ICoreWebView2Settings6> swipe;
    if (SUCCEEDED(settings.As(&swipe))) {
        swipe->put_IsSwipeNavigationEnabled(FALSE);
    }
#ifdef NDEBUG
    settings->put_AreDefaultContextMenusEnabled(FALSE);
    settings->put_AreDevToolsEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> keySettings;
    if (SUCCEEDED(settings.As(&keySettings))) {
        keySettings->put_AreBrowserAcceleratorKeysEnabled(FALSE); // No F5 reload, Ctrl+F find.
    }
#endif

    controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [this](ICoreWebView2Controller*,
                   ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                acceleratorKeyPressed(args);
                return S_OK;
            })
            .Get(),
        &acceleratorToken);
    core->AddScriptToExecuteOnDocumentCreated(kKeyScript, nullptr);
    core->AddWebResourceRequestedFilter(L"orizon://*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    core->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                resourceRequested(args);
                return S_OK;
            })
            .Get(),
        &resourceToken);
    core->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                messageReceived(args);
                return S_OK;
            })
            .Get(),
        &messageToken);
    core->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                if (uri == nullptr || !std::wstring_view(uri).starts_with(kUiOrigin)) {
                    args->put_Cancel(TRUE);
                }
                CoTaskMemFree(uri);
                return S_OK;
            })
            .Get(),
        &navigationToken);
    core->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE); // No windows of its own.
                return S_OK;
            })
            .Get(),
        &newWindowToken);
    core->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                processFailed(args);
                return S_OK;
            })
            .Get(),
        &processFailedToken);
    core->Navigate(kStartUrl);

    for (const std::wstring& message : queuedMessages) {
        core->PostWebMessageAsString(message.c_str());
    }
    queuedMessages.clear();
}

void WebViewWin::setSize(int newWidth, int newHeight)
{
    width = newWidth;
    height = newHeight;
    const DpiContext dpi(parent);
    SetWindowPos(container, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    if (controller) {
        controller->put_Bounds(RECT{0, 0, width, height});
    }
}

void WebViewWin::setScale(double newScale)
{
    scale = newScale;
    ComPtr<ICoreWebView2Controller3> scaling;
    if (controller && SUCCEEDED(controller.As(&scaling))) {
        scaling->put_RasterizationScale(scale);
    }
}

void WebViewWin::postMessage(std::string_view message)
{
    std::wstring text = widen(message);
    if (!core) {
        queuedMessages.push_back(std::move(text));
        return;
    }
    core->PostWebMessageAsString(text.c_str());
}

void WebViewWin::resourceRequested(ICoreWebView2WebResourceRequestedEventArgs* args)
{
    ComPtr<ICoreWebView2WebResourceRequest> request;
    args->get_Request(&request);
    LPWSTR uri = nullptr;
    request->get_Uri(&uri);
    const std::wstring url = uri != nullptr ? uri : L"";
    CoTaskMemFree(uri);

    if (url.starts_with(kFrameUrl)) {
        requestFrame(args);
        return;
    }
    // orizon://ui/<path>: the path names an embedded asset; the bare origin is index.html.
    constexpr std::wstring_view kOrigin = L"orizon://ui/";
    std::wstring path = url.starts_with(kOrigin) ? url.substr(kOrigin.size()) : L"";
    path = path.substr(0, path.find_first_of(L"?#"));
    const EmbeddedAsset* asset = findEmbeddedAsset(path.empty() ? "index.html" : narrow(path));
    if (asset == nullptr) {
        answer(args, nullptr, 0, L"text/plain", 404);
        return;
    }
    answer(args, asset->data, asset->size, widen(asset->mimeType).c_str());
}

void WebViewWin::answer(ICoreWebView2WebResourceRequestedEventArgs* args, const void* data,
                        std::size_t size, const wchar_t* contentType, int status)
{
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(static_cast<const BYTE*>(data), static_cast<UINT>(size)));
    const std::wstring headers =
        std::wstring(L"Content-Type: ") + contentType + L"\r\nCache-Control: no-store";
    ComPtr<ICoreWebView2WebResourceResponse> response;
    environment->CreateWebResourceResponse(
        stream.Get(), status, status == 200 ? L"OK" : L"Not Found", headers.c_str(), &response);
    args->put_Response(response.Get());
}

// Frames travel as the binary body of the page's fetch: no text encoding on either side.
void WebViewWin::sendFrame(std::span<const std::byte> frame)
{
    std::vector<std::byte> bytes(frame.begin(), frame.end());
    if (!frameRequest) {
        heldFrame = std::move(bytes);
        hasHeldFrame = true;
        return;
    }
    answerFrame(bytes);
}

void WebViewWin::requestFrame(ComPtr<ICoreWebView2WebResourceRequestedEventArgs> args)
{
    if (frameRequest) {
        answerFrame({}); // Superseded; the page asks for one frame at a time.
    }
    frameRequest = std::move(args);
    frameRequest->GetDeferral(&frameDeferral);
    if (hasHeldFrame) {
        answerFrame(heldFrame);
        heldFrame.clear();
        hasHeldFrame = false;
    }
    if (onFrameRequest) {
        onFrameRequest(*this);
    }
}

void WebViewWin::answerFrame(const std::vector<std::byte>& frame)
{
    answer(frameRequest.Get(), frame.data(), frame.size(), L"application/octet-stream");
    frameDeferral->Complete();
    frameRequest.Reset();
    frameDeferral.Reset();
}

void WebViewWin::messageReceived(ICoreWebView2WebMessageReceivedEventArgs* args)
{
    LPWSTR source = nullptr;
    const bool isFromUi = SUCCEEDED(args->get_Source(&source)) && source != nullptr &&
                          std::wstring_view(source).starts_with(kUiOrigin);
    CoTaskMemFree(source);
    if (!isFromUi) {
        return;
    }
    LPWSTR text = nullptr;
    if (FAILED(args->TryGetWebMessageAsString(&text)) || text == nullptr) {
        return;
    }
    const std::wstring body = text;
    CoTaskMemFree(text);
    if (body.starts_with(kFocusPrefix)) {
        isEditableFocused = body.substr(kFocusPrefix.size()) == L"1";
        return;
    }
    if (body.starts_with(kKeyPrefix)) {
        forwardPageKey(std::wstring_view(body).substr(kKeyPrefix.size()));
        return;
    }
    if (onMessage) {
        onMessage(*this, narrow(body));
    }
}

// Posted, not sent, so that the key passes through the host's message loop and its
// shortcut handling, as a key typed into its own window would.
void WebViewWin::acceleratorKeyPressed(ICoreWebView2AcceleratorKeyPressedEventArgs* args)
{
    if (isEditableFocused) {
        return;
    }
    COREWEBVIEW2_KEY_EVENT_KIND kind{};
    UINT key = 0;
    INT keyData = 0;
    args->get_KeyEventKind(&kind);
    args->get_VirtualKey(&key);
    args->get_KeyEventLParam(&keyData);
    UINT message = WM_KEYDOWN;
    switch (kind) {
    case COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN:
        message = WM_KEYDOWN;
        break;
    case COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP:
        message = WM_KEYUP;
        break;
    case COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN:
        message = WM_SYSKEYDOWN;
        break;
    case COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_UP:
        message = WM_SYSKEYUP;
        break;
    }
    args->put_Handled(TRUE); // Neither the page nor the browser acts on it.
    PostMessageW(parent, message, key, static_cast<LPARAM>(keyData));
}

// A key the page reported, rebuilt as the window message the host would have received.
void WebViewWin::forwardPageKey(std::wstring_view report)
{
    const std::size_t separator = report.find(L':');
    if (separator == std::wstring_view::npos || separator == 0) {
        return;
    }
    const bool isDown = report[0] == L'd';
    const bool isRepeat = report.substr(0, separator).find(L'r') != std::wstring_view::npos;
    const auto key = static_cast<UINT>(std::wcstoul(report.data() + separator + 1, nullptr, 10));
    if (key == 0 || key > 0xFF) {
        return;
    }
    // Repeat count 1, the scan code, then the previous and the transition state.
    auto keyData = static_cast<LPARAM>(1 | (MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16));
    if (!isDown) {
        keyData |= LPARAM{3} << 30;
    } else if (isRepeat) {
        keyData |= LPARAM{1} << 30;
    }
    PostMessageW(parent, isDown ? WM_KEYDOWN : WM_KEYUP, key, keyData);
}

bool WebViewWin::terminateProcessForTests(bool browser)
{
    if (!core) {
        return false;
    }
    UINT32 id = 0;
    if (browser) {
        core->get_BrowserProcessId(&id);
    } else {
        ComPtr<ICoreWebView2Environment8> processes;
        ComPtr<ICoreWebView2ProcessInfoCollection> infos;
        UINT32 count = 0;
        if (FAILED(environment.As(&processes)) || FAILED(processes->GetProcessInfos(&infos)) ||
            FAILED(infos->get_Count(&count))) {
            return false;
        }
        for (UINT32 index = 0; index < count && id == 0; ++index) {
            ComPtr<ICoreWebView2ProcessInfo> info;
            COREWEBVIEW2_PROCESS_KIND kind{};
            if (SUCCEEDED(infos->GetValueAtIndex(index, &info)) &&
                SUCCEEDED(info->get_Kind(&kind)) && kind == COREWEBVIEW2_PROCESS_KIND_RENDERER) {
                INT32 processId = 0;
                info->get_ProcessId(&processId);
                id = static_cast<UINT32>(processId);
            }
        }
    }
    HANDLE process = id != 0 ? OpenProcess(PROCESS_TERMINATE, FALSE, id) : nullptr;
    const bool ended = process != nullptr && TerminateProcess(process, 1) != FALSE;
    if (process != nullptr) {
        CloseHandle(process);
    }
    return ended;
}

// ExecuteScript reports the result as JSON; only a string result is passed on.
void WebViewWin::evaluate(std::string_view script, std::function<void(std::string)> done)
{
    if (!core) {
        done("");
        return;
    }
    core->ExecuteScript(
        widen(script).c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([done = std::move(done)](
                                                                 HRESULT result,
                                                                 LPCWSTR json) -> HRESULT {
            std::string value;
            if (SUCCEEDED(result) && json != nullptr) {
                const auto parsed = nlohmann::json::parse(narrow(json), nullptr, false);
                if (parsed.is_string()) {
                    value = parsed.get<std::string>();
                }
            }
            done(value);
            return S_OK;
        }).Get());
}

} // namespace

void setBrowserFolderForTests(std::wstring folder)
{
    browserFolderForTests() = std::move(folder);
}

bool terminateProcessForTests(WebView& view, bool browser)
{
    auto* windows = dynamic_cast<WebViewWin*>(&view);
    return windows != nullptr && windows->terminateProcessForTests(browser);
}

std::unique_ptr<WebView> WebView::create(void* parent, int width, int height,
                                         MessageHandler onMessage,
                                         FrameRequestHandler onFrameRequest)
{
    return std::make_unique<WebViewWin>(static_cast<HWND>(parent), width, height,
                                        std::move(onMessage), std::move(onFrameRequest));
}

} // namespace orizon::editor
