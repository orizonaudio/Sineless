#include "FitStates.h"
#include "editor/DisplayScale.h"
#include "editor/Editor.h"
#include "editor/WebView.h"
#include "engine/Engine.h"
#include "win/WebViewWinTesting.h"

// windows.h first: the other Windows headers depend on it.
// clang-format off
#define NOMINMAX
#include <windows.h>
// clang-format on
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// The Windows counterpart of EditorTests.mm: the editor in a real top-level window, with
// WebView2, the embedded UI and the session over the native channel.

namespace orizon::editor {
namespace {

using namespace Steinberg;

// WebView2's first start in a fresh profile takes a few seconds.
constexpr std::chrono::seconds kPageTimeout{60};

bool pumpUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
}

// Runs the enclosed code in the given DPI awareness, as a host opening its plugin window.
class ScopedDpiAwareness
{
public:
    explicit ScopedDpiAwareness(DPI_AWARENESS_CONTEXT context)
        : previous(SetThreadDpiAwarenessContext(context))
    {}
    ~ScopedDpiAwareness() { SetThreadDpiAwarenessContext(previous); }
    ScopedDpiAwareness(const ScopedDpiAwareness&) = delete;
    ScopedDpiAwareness& operator=(const ScopedDpiAwareness&) = delete;

private:
    DPI_AWARENESS_CONTEXT previous;
};

class EditorTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        // WebView2 runs in a single-threaded apartment, like a host's UI thread.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &EditorTest::windowProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"OrizonEditorTest";
        RegisterClassExW(&windowClass);
    }

    // Stands in for the host's window and counts the keystrokes forwarded to it.
    static LRESULT CALLBACK windowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_KEYDOWN) {
            ++hostKeyDowns;
            return 0;
        }
        return DefWindowProcW(handle, message, wParam, lParam);
    }

    static inline int hostKeyDowns = 0;

    // Shown, as a host's editor window is: the editor pauses the page while it is hidden.
    static HWND makeWindow()
    {
        HWND window = CreateWindowExW(0, L"OrizonEditorTest", L"Sineless test", WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, Editor::kDefaultWidth,
                                      Editor::kDefaultHeight, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        return window;
    }

    // Sizes the window's client area to the editor, as a host does, then attaches the editor.
    static tresult attach(HWND window, Editor& editor)
    {
        RECT frame{0, 0, editor.getRect().getWidth(), editor.getRect().getHeight()};
        AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(window));
        SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return editor.attached(window, kPlatformTypeHWND);
    }

    IPtr<Editor>
    makeEditor(std::vector<std::string>& received,
               std::shared_ptr<engine::Engine> engine = std::make_shared<engine::Engine>())
    {
        return owned(new Editor(std::move(engine), [&received](std::string_view message) {
            received.emplace_back(message);
        }));
    }

    // Evaluates `script` in the page and returns its result as a string ("" if none yet).
    static std::string evaluate(WebView& view, const std::string& script)
    {
        struct Result
        {
            bool done = false;
            std::string value;
        };
        auto result = std::make_shared<Result>();
        view.evaluate(script, [result](std::string value) {
            result->value = std::move(value);
            result->done = true;
        });
        pumpUntil([&] { return result->done; }, std::chrono::seconds(5));
        return result->value;
    }

    static bool hasSnapshot(WebView& view)
    {
        return evaluate(view, "String(window.orizonDebug?.snapshot?.type ?? '')") == "snapshot";
    }

    static bool saw(const std::vector<std::string>& messages, const std::string& type)
    {
        return std::ranges::any_of(messages, [&](const std::string& message) {
            return message.find(R"("type":")" + type + "\"") != std::string::npos;
        });
    }
};

TEST_F(EditorTest, SupportsOnlyHwnd)
{
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    EXPECT_EQ(editor->isPlatformTypeSupported(kPlatformTypeHWND), kResultTrue);
    EXPECT_EQ(editor->isPlatformTypeSupported(kPlatformTypeNSView), kResultFalse);
}

// The page loads from the embedded UI over orizon:// and completes the session handshake.
TEST_F(EditorTest, LoadsThePageAndCompletesTheHandshake)
{
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(*editor->webView()); }, kPageTimeout))
        << "the page did not receive the session snapshot";
    EXPECT_TRUE(saw(received, "hello"));
    EXPECT_EQ(editor->removed(), kResultOk);
    EXPECT_EQ(editor->webView(), nullptr);
    DestroyWindow(window);
}

// The page runs only the script it was built with: a script put into it some other way (a host
// track name that ever reached markup, say) is refused. Its own stylesheet applies, and every font
// it declares loads.
TEST_F(EditorTest, RunsNoScriptThePageWasNotBuiltWith)
{
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    WebView& view = *editor->webView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
    EXPECT_EQ(evaluate(view, "(() => {"
                             "  const script = document.createElement('script');"
                             "  script.textContent = 'window.injected = true';"
                             "  document.head.append(script);"
                             "  return String(window.injected === true);"
                             "})()"),
              "false");
    evaluate(view, "(document.styleSheets.length > 0 && document.fonts.size > 0"
                   "   ? Promise.all([...document.fonts].map((font) => font.load()))"
                   "   : Promise.reject()).then("
                   "  () => { window.fonts = 'loaded'; }, () => { window.fonts = 'failed'; });"
                   "''");
    EXPECT_TRUE(pumpUntil([&] { return evaluate(view, "String(window.fonts ?? '')") == "loaded"; },
                          kPageTimeout));
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

// Frames flow as the page asks for them, one per request, as the binary body of its fetch.
TEST_F(EditorTest, StreamsFramesOnePerRequest)
{
    auto engine = std::make_shared<engine::Engine>();
    engine->prepare(48000.0);
    engine->settings().set(protocol::SettingId::kTimeBase, 0.0);    // Seconds.
    engine->settings().set(protocol::SettingId::kTriggerMode, 2.0); // Free: a frame per request.
    std::vector<float> tone(96000);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = (i / 50) % 2 == 0 ? 0.5f : -0.5f;
    }
    std::array<const float*, 2> channels{tone.data(), tone.data()};
    engine->captureRing().write(channels.data(), 2, static_cast<int>(tone.size()));

    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received, engine);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    WebView& view = *editor->webView();
    const auto frameCount = [&] {
        return std::stoi("0" + evaluate(view, "String(window.orizonDebug?.frameCount ?? 0)"));
    };
    ASSERT_TRUE(pumpUntil([&] { return frameCount() >= 1; }, kPageTimeout))
        << "the page asks for its first frame on start";
    for (int request = 0; request < 4; ++request) {
        const int before = frameCount();
        engine->captureRing().write(channels.data(), 2, 480); // New audio, as a host would.
        evaluate(view, "window.orizonDebug.requestFrame(); ''");
        ASSERT_TRUE(pumpUntil([&] { return frameCount() > before; }, std::chrono::seconds(5)))
            << "a frame per request";
    }
    EXPECT_EQ(evaluate(view, "String(window.orizonDebug.lastFrame.views.size)"), "2");
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

TEST_F(EditorTest, ForwardsKeysToHostUnlessEditableElementHasFocus)
{
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    WebView& view = *editor->webView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
    // Presses a key in the page, waits for any forwarded key to arrive, and returns how many
    // the host has seen.
    const auto press = [&] {
        evaluate(view, "document.body.dispatchEvent("
                       "new KeyboardEvent('keydown', {keyCode: 65, bubbles: true})); ''");
        pumpUntil([] { return false; }, std::chrono::milliseconds(300));
        return hostKeyDowns;
    };
    hostKeyDowns = 0;

    EXPECT_EQ(press(), 1) << "key should reach the host while nothing is focused";

    evaluate(view, "const input = document.createElement('input');"
                   "input.id = 'keyTest'; document.body.append(input); input.focus(); ''");
    EXPECT_EQ(press(), 1) << "key should stay in the page while an input is focused";

    evaluate(view, "document.getElementById('keyTest').blur(); ''");
    EXPECT_EQ(press(), 2) << "key should reach the host again after blur";

    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

// Stands in for the host's window: records the sizes the editor asks for and grants them.
class TestFrame final : public IPlugFrame
{
public:
    tresult PLUGIN_API queryInterface(const TUID queried, void** obj) override
    {
        if (FUnknownPrivate::iidEqual(queried, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual(queried, FUnknown::iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* newSize) override
    {
        requested = *newSize;
        ++requests;
        return view->onSize(newSize);
    }

    ViewRect requested;
    int requests = 0;
};

// The resize grip's size is in CSS pixels: the editor asks the host for it in window units, so
// the page gets the size the user dragged to whatever the display scaling. Meaningful on a
// display scaled above 100 %; the scale is printed.
TEST_F(EditorTest, AsksTheHostForTheGripSizeInWindowUnits)
{
    const ScopedDpiAwareness awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND window = makeWindow();
    TestFrame frame; // Outlives the editor, which keeps a pointer to it.
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    editor->setFrame(&frame);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    WebView& view = *editor->webView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
    const double scale = displayScale(window);
    std::cout << "display scale " << scale << "\n";
    const int requestsBefore = frame.requests;

    const int width = Editor::kDefaultWidth + 300;
    const int height = Editor::kDefaultHeight + 200;
    evaluate(view, "window.chrome.webview.postMessage(JSON.stringify({type: 'resize', width: " +
                       std::to_string(width) + ", height: " + std::to_string(height) + "})); ''");
    ASSERT_TRUE(pumpUntil([&] { return frame.requests > requestsBefore; }, kPageTimeout));
    EXPECT_EQ(frame.requested.getWidth(), static_cast<int>(std::ceil(width * scale - 1e-6)));
    EXPECT_EQ(frame.requested.getHeight(), static_cast<int>(std::ceil(height * scale - 1e-6)));
    EXPECT_TRUE(
        pumpUntil([&] { return evaluate(view, "String(innerWidth)") == std::to_string(width); },
                  kPageTimeout))
        << "the page has the width dragged to, in CSS pixels";
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

// The page lays out in CSS pixels at the editor's default size whatever the display scaling, in
// a DPI-aware host window (sized in physical pixels) and in an unaware one (sized in 96-dpi
// units, which WebView2 still draws sharp, at the display's scale). Meaningful on a display
// scaled above 100 %; the scale is printed.
TEST_F(EditorTest, LaysOutAtTheDefaultCssSizeAtAnyDisplayScale)
{
    for (const auto context :
         {DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, DPI_AWARENESS_CONTEXT_UNAWARE}) {
        const ScopedDpiAwareness awareness(context);
        HWND window = makeWindow();
        std::vector<std::string> received;
        auto editor = makeEditor(received);
        ASSERT_EQ(attach(window, *editor), kResultTrue);
        WebView& view = *editor->webView();
        ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
        const double scale = displayScale(window);
        std::cout << "display scale " << scale << "\n";
        EXPECT_EQ(editor->scaleFactor(), scale);
        EXPECT_EQ(editor->getRect().getWidth(),
                  static_cast<int>(std::ceil(Editor::kDefaultWidth * scale - 1e-6)));
        EXPECT_EQ(evaluate(view, "String(innerWidth)"), std::to_string(Editor::kDefaultWidth));
        double displayed = scale;
        if (context == DPI_AWARENESS_CONTEXT_UNAWARE) {
            const ScopedDpiAwareness perMonitor(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            displayed = GetDpiForSystem() / 96.0; // The display's scale, with one display.
        }
        EXPECT_EQ(std::stod("0" + evaluate(view, "String(devicePixelRatio)")), displayed);
        EXPECT_TRUE(AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), context))
            << "the host's thread keeps its DPI awareness";
        EXPECT_EQ(editor->removed(), kResultOk);
        DestroyWindow(window);
    }
}

// With the embedded fonts as Windows renders them, everything fits at the minimum size, in
// every state that changes the labels' widths.
TEST_F(EditorTest, FitsTheMinimumSizeInEveryState)
{
    const ScopedDpiAwareness awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto engine = std::make_shared<engine::Engine>();
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received, engine);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    WebView& view = *editor->webView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
    ViewRect minimum(0, 0, 0, 0);
    editor->checkSizeConstraint(&minimum);
    ASSERT_EQ(editor->onSize(&minimum), kResultTrue);
    ASSERT_TRUE(pumpUntil(
        [&] {
            return evaluate(view, "String(innerWidth)") == std::to_string(Editor::kMinimumWidth);
        },
        std::chrono::seconds(5)));
    for (const fit::State& state : fit::states()) {
        engine::Settings settings;
        for (const auto& [id, value] : state.settings) {
            settings.set(id, value);
        }
        engine->settings().replace(settings);
        pumpUntil([] { return false; }, std::chrono::milliseconds(400)); // A state sync or more.
        if (!state.script.empty()) {
            evaluate(view, std::string(state.script));
            pumpUntil([] { return false; }, std::chrono::milliseconds(200));
        }
        EXPECT_EQ(evaluate(view, fit::kOverflowScript), "") << state.name;
        EXPECT_LE(std::stoi(evaluate(view, fit::kChromeScript)), Editor::kChromeHeight)
            << state.name;
    }
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

// Hosts hide the editor's window when it is closed or the host is minimised; WebView2 cannot
// tell by itself, and the page would keep drawing.
TEST_F(EditorTest, PausesThePageWhileHidden)
{
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    WebView& view = *editor->webView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
    evaluate(view,
             "window.testFrames = 0;"
             "(function count() { window.testFrames++; requestAnimationFrame(count); })(); ''");
    const auto frames = [&] {
        return std::stoi("0" + evaluate(view, "String(window.testFrames)"));
    };
    const auto state = [&] { return evaluate(view, "document.visibilityState"); };

    ShowWindow(window, SW_HIDE);
    ASSERT_TRUE(pumpUntil([&] { return state() == "hidden"; }, std::chrono::seconds(3)));
    const int whileHidden = frames();
    pumpUntil([] { return false; }, std::chrono::milliseconds(500));
    EXPECT_LE(frames() - whileHidden, 1) << "no drawing while hidden";

    ShowWindow(window, SW_SHOWNOACTIVATE);
    ASSERT_TRUE(pumpUntil([&] { return state() == "visible"; }, std::chrono::seconds(3)));
    const int shown = frames();
    EXPECT_TRUE(pumpUntil([&] { return frames() > shown + 10; }, std::chrono::seconds(3)))
        << "drawing again once shown";
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

// When WebView2 cannot start (here: its browser is looked for in a folder that does not exist,
// as without the runtime), the editor says so instead of staying empty.
// (Not WEBVIEW2_BROWSER_EXECUTABLE_FOLDER: WebView2 ignores it in elevated processes, which CI
// runs.)
TEST_F(EditorTest, ExplainsWhenWebView2CannotStart)
{
    setBrowserFolderForTests(L"C:\\Sineless-no-such-folder");
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    HWND container = FindWindowExW(window, nullptr, L"OrizonSinelessWebViewHost", nullptr);
    ASSERT_NE(container, nullptr);
    std::wstring text;
    ASSERT_TRUE(pumpUntil(
        [&] {
            std::array<wchar_t, 512> buffer{};
            text.assign(buffer.data(),
                        static_cast<std::size_t>(GetWindowTextW(container, buffer.data(),
                                                                static_cast<int>(buffer.size()))));
            return !text.empty();
        },
        kPageTimeout))
        << "no message while WebView2 cannot start";
    EXPECT_NE(text.find(L"WebView2"), std::wstring::npos);
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
    setBrowserFolderForTests({});
}

// The page stays on the embedded UI: a link elsewhere and a new window are refused.
TEST_F(EditorTest, StaysOnTheEmbeddedUi)
{
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    WebView& view = *editor->webView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
    evaluate(view,
             "window.open('https://example.com/'); location.href = 'https://example.com/'; ''");
    pumpUntil([] { return false; }, std::chrono::milliseconds(1500));
    EXPECT_EQ(evaluate(view, "location.href"), "orizon://ui/index.html");
    EXPECT_TRUE(hasSnapshot(view));
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

// A crashed renderer is reloaded, and the new page gets the session again.
TEST_F(EditorTest, ReloadsAfterTheRendererCrashes)
{
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    WebView& view = *editor->webView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
    const auto hellos = [&] {
        return std::ranges::count_if(received, [](const std::string& message) {
            return message.find(R"("type":"hello")") != std::string::npos;
        });
    };
    ASSERT_EQ(hellos(), 1);
    ASSERT_TRUE(terminateProcessForTests(view, false));
    EXPECT_TRUE(pumpUntil([&] { return hellos() == 2 && hasSnapshot(view); }, kPageTimeout))
        << "the page did not come back";
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

// A renderer that keeps crashing is not reloaded forever: after three reloads in a minute the
// editor says so instead.
TEST_F(EditorTest, ExplainsWhenThePageKeepsFailing)
{
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    WebView& view = *editor->webView();
    const auto hellos = [&] {
        return std::ranges::count_if(received, [](const std::string& message) {
            return message.find(R"("type":"hello")") != std::string::npos;
        });
    };
    for (std::ptrdiff_t reload = 0; reload < 3; ++reload) {
        ASSERT_TRUE(
            pumpUntil([&] { return hellos() == reload + 1 && hasSnapshot(view); }, kPageTimeout))
            << "reload " << reload;
        ASSERT_TRUE(terminateProcessForTests(view, false));
    }
    ASSERT_TRUE(pumpUntil([&] { return hellos() == 4 && hasSnapshot(view); }, kPageTimeout));
    HWND container = FindWindowExW(window, nullptr, L"OrizonSinelessWebViewHost", nullptr);
    ASSERT_TRUE(terminateProcessForTests(view, false));
    std::wstring text;
    EXPECT_TRUE(pumpUntil(
        [&] {
            std::array<wchar_t, 512> buffer{};
            text.assign(buffer.data(),
                        static_cast<std::size_t>(GetWindowTextW(container, buffer.data(),
                                                                static_cast<int>(buffer.size()))));
            return text.find(L"keeps failing") != std::wstring::npos;
        },
        std::chrono::seconds(10)));
    pumpUntil([] { return false; }, std::chrono::seconds(2));
    EXPECT_EQ(hellos(), 4) << "reloaded past the limit";
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

// When WebView2's browser exits, every view of it is gone: the editor says so.
TEST_F(EditorTest, ExplainsWhenTheBrowserExits)
{
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    WebView& view = *editor->webView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
    HWND container = FindWindowExW(window, nullptr, L"OrizonSinelessWebViewHost", nullptr);
    ASSERT_TRUE(terminateProcessForTests(view, true));
    std::wstring text;
    EXPECT_TRUE(pumpUntil(
        [&] {
            std::array<wchar_t, 512> buffer{};
            text.assign(buffer.data(),
                        static_cast<std::size_t>(GetWindowTextW(container, buffer.data(),
                                                                static_cast<int>(buffer.size()))));
            return text.find(L"stopped") != std::wstring::npos;
        },
        std::chrono::seconds(20)));
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

TEST_F(EditorTest, ExplainsWhenTheEngineIsUnavailable)
{
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received, nullptr);
    ASSERT_EQ(attach(window, *editor), kResultTrue);
    EXPECT_TRUE(pumpUntil(
        [&] {
            return evaluate(*editor->webView(), "String(window.orizonDebug?.error?.code ?? '')") ==
                   "engineUnavailable";
        },
        kPageTimeout));
    EXPECT_EQ(editor->removed(), kResultOk);
    DestroyWindow(window);
}

// Opening and closing repeatedly, including closing before WebView2 is ready, leaves nothing
// behind and works every time.
TEST_F(EditorTest, RoundTripsMessagesAcrossOpenCloseCycles)
{
    constexpr int kCycles = 10;
    DWORD handlesAtStart = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        if (cycle == 2) { // After WebView2's one-time setup.
            GetProcessHandleCount(GetCurrentProcess(), &handlesAtStart);
        }
        HWND window = makeWindow();
        std::vector<std::string> received;
        auto editor = makeEditor(received);
        ASSERT_EQ(attach(window, *editor), kResultTrue);
        if (cycle % 2 == 1) {
            EXPECT_EQ(editor->removed(), kResultOk) << "closed while WebView2 starts";
        } else {
            ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(*editor->webView()); }, kPageTimeout))
                << "cycle " << cycle;
            EXPECT_EQ(editor->removed(), kResultOk);
        }
        DestroyWindow(window);
        pumpUntil([] { return false; }, std::chrono::milliseconds(50));
    }
    pumpUntil([] { return false; }, std::chrono::seconds(1)); // WebView2 winds down.
    DWORD handlesAtEnd = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handlesAtEnd);
    std::cout << "process handles " << handlesAtStart << " -> " << handlesAtEnd << "\n";
    // WebView2 still leaves handles to its exited processes for views closed before they have
    // finished starting, or while another starts: 2.5 to 3 per cycle here, where half the
    // editors close that early, as a user hardly can. Guards against more.
    EXPECT_LE(static_cast<int>(handlesAtEnd) - static_cast<int>(handlesAtStart), 4 * (kCycles - 2))
        << "handles left behind by " << kCycles - 2 << " open/close cycles";
}

// As a user opens and closes the editor: each one loads before it closes. WebView2 leaves a
// handle behind for each view closed while visible; the editor hides it first.
TEST_F(EditorTest, LeavesNoHandlesBehindWhenClosedAfterLoading)
{
    constexpr int kCycles = 32;
    DWORD handlesAtStart = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        if (cycle == 2) { // After WebView2's one-time setup.
            GetProcessHandleCount(GetCurrentProcess(), &handlesAtStart);
        }
        HWND window = makeWindow();
        std::vector<std::string> received;
        auto editor = makeEditor(received);
        ASSERT_EQ(attach(window, *editor), kResultTrue);
        ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(*editor->webView()); }, kPageTimeout))
            << "cycle " << cycle;
        EXPECT_EQ(editor->removed(), kResultOk);
        DestroyWindow(window);
        pumpUntil([] { return false; }, std::chrono::milliseconds(50));
    }
    pumpUntil([] { return false; }, std::chrono::seconds(1)); // WebView2 winds down.
    DWORD handlesAtEnd = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handlesAtEnd);
    std::cout << "process handles " << handlesAtStart << " -> " << handlesAtEnd << "\n";
    // The count moves with WebView2's own processes, by up to about 12 either way without any
    // leak (-12 to +4 over ten runs in a Windows 11 VM, +7 once on CI): half a handle per cycle
    // stays above that noise, and below the one per cycle a handle left behind by each editor
    // would add.
    EXPECT_LE(static_cast<int>(handlesAtEnd) - static_cast<int>(handlesAtStart), (kCycles - 2) / 2)
        << "handles left behind by " << kCycles - 2 << " open/close cycles";
}

} // namespace
} // namespace orizon::editor
