#include "EmbeddedAssets.h"
#include "editor/WebView.h"

// windows.h first: the other Windows headers depend on it. Not the lean variant: WebView2.h needs
// its COM declarations.
// clang-format off
#define NOMINMAX
#include <windows.h>
// clang-format on
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <cwchar>
#include <deque>
#include <nlohmann/json.hpp>
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

namespace orizon::editor {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kStartUrl = L"orizon://ui/index.html";
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
// %LOCALAPPDATA%\Orizon Audio\O-Scope\WebView2.
std::wstring userDataFolder()
{
    PWSTR localAppData = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        folder = std::wstring(localAppData) + L"\\Orizon Audio\\O-Scope\\WebView2";
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
    void postMessage(std::string_view message) override;
    void sendFrame(std::span<const std::byte> frame) override;
    [[nodiscard]] void* nativeView() const override { return parent; }
    void evaluate(std::string_view script, std::function<void(std::string)> done) override;

private:
    // Lets WebView2's completion handlers, which may run after this object is gone, find it.
    struct Link
    {
        WebViewWin* owner = nullptr;
    };

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
    int width = 0;
    int height = 0;
    MessageHandler onMessage;
    FrameRequestHandler onFrameRequest;
    std::shared_ptr<Link> link = std::make_shared<Link>();
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> core;
    EventRegistrationToken resourceToken{};
    EventRegistrationToken messageToken{};
    EventRegistrationToken acceleratorToken{};
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
    const std::wstring folder = userDataFolder();
    CreateCoreWebView2EnvironmentWithOptions(
        nullptr, folder.empty() ? nullptr : folder.c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [link = link](HRESULT result, ICoreWebView2Environment* created) -> HRESULT {
                if (SUCCEEDED(result) && link->owner != nullptr) {
                    link->owner->environmentCreated(created);
                }
                return S_OK;
            })
            .Get());
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
    }
    if (controller) {
        controller->remove_AcceleratorKeyPressed(acceleratorToken);
        controller->Close();
    }
}

void WebViewWin::environmentCreated(ICoreWebView2Environment* created)
{
    environment = created;
    environment->CreateCoreWebView2Controller(
        parent, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [link = link](HRESULT result, ICoreWebView2Controller* made) -> HRESULT {
                        if (SUCCEEDED(result) && link->owner != nullptr) {
                            link->owner->controllerCreated(made);
                        }
                        return S_OK;
                    })
                    .Get());
}

void WebViewWin::controllerCreated(ICoreWebView2Controller* created)
{
    controller = created;
    controller->get_CoreWebView2(&core);
    setSize(width, height);

    ComPtr<ICoreWebView2Settings> settings;
    core->get_Settings(&settings);
    settings->put_IsStatusBarEnabled(FALSE);
    settings->put_IsZoomControlEnabled(FALSE); // Ctrl + scroll zooms the trace, not the page.
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
    if (controller) {
        controller->put_Bounds(RECT{0, 0, width, height});
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

std::unique_ptr<WebView> WebView::create(void* parent, int width, int height,
                                         MessageHandler onMessage,
                                         FrameRequestHandler onFrameRequest)
{
    return std::make_unique<WebViewWin>(static_cast<HWND>(parent), width, height,
                                        std::move(onMessage), std::move(onFrameRequest));
}

} // namespace orizon::editor
