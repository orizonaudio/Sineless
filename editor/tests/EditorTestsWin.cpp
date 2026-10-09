#include "editor/DisplayScale.h"
#include "editor/Editor.h"
#include "editor/WebView.h"
#include "engine/Engine.h"

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

// Lists the page's elements that reach past its right edge or clip their content, "" if none.
constexpr const char* kOverflowScript = R"JS(
(() => {
  const found = [];
  for (const element of document.querySelectorAll("body *")) {
    const box = element.getBoundingClientRect();
    if (box.width === 0) continue;
    const name = element.id || element.className || element.tagName;
    if (box.right > innerWidth + 0.5) {
      found.push(name + " ends at " + Math.round(box.right));
    } else if (element.scrollWidth > element.clientWidth + 1 &&
               getComputedStyle(element).overflowX !== "visible") {
      found.push(name + " clips its content");
    }
  }
  return found.slice(0, 8).join("; ");
})()
)JS";

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
        HWND window = CreateWindowExW(0, L"OrizonEditorTest", L"O-Scope test", WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, Editor::kDefaultWidth,
                                      Editor::kDefaultHeight, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        return window;
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
    ASSERT_EQ(editor->attached(window, kPlatformTypeHWND), kResultTrue);
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(*editor->webView()); }, kPageTimeout))
        << "the page did not receive the session snapshot";
    EXPECT_TRUE(saw(received, "hello"));
    EXPECT_EQ(editor->removed(), kResultOk);
    EXPECT_EQ(editor->webView(), nullptr);
    DestroyWindow(window);
}

// Frames flow as the page asks for them, one per request, as the binary body of its fetch.
TEST_F(EditorTest, StreamsFramesOnePerRequest)
{
    auto engine = std::make_shared<engine::Engine>();
    engine->setSampleRate(48000.0);
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
    ASSERT_EQ(editor->attached(window, kPlatformTypeHWND), kResultTrue);
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
    ASSERT_EQ(editor->attached(window, kPlatformTypeHWND), kResultTrue);
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

// The page lays out in CSS pixels at the editor's default size whatever the display scaling, in
// a DPI-aware host window (sized in physical pixels) and in an unaware one (stretched by
// Windows). Meaningful on a display scaled above 100 %; the scale is printed.
TEST_F(EditorTest, LaysOutAtTheDefaultCssSizeAtAnyDisplayScale)
{
    for (const auto context :
         {DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, DPI_AWARENESS_CONTEXT_UNAWARE}) {
        const ScopedDpiAwareness awareness(context);
        HWND window = makeWindow();
        std::vector<std::string> received;
        auto editor = makeEditor(received);
        ASSERT_EQ(editor->attached(window, kPlatformTypeHWND), kResultTrue);
        WebView& view = *editor->webView();
        ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
        const double scale = displayScale(window);
        std::cout << "display scale " << scale << "\n";
        EXPECT_EQ(editor->scaleFactor(), scale);
        EXPECT_EQ(editor->getRect().getWidth(),
                  static_cast<int>(std::ceil(Editor::kDefaultWidth * scale - 1e-6)));
        EXPECT_EQ(evaluate(view, "String(innerWidth)"), std::to_string(Editor::kDefaultWidth));
        EXPECT_EQ(std::stod("0" + evaluate(view, "String(devicePixelRatio)")), scale);
        EXPECT_EQ(editor->removed(), kResultOk);
        DestroyWindow(window);
    }
}

// With the embedded fonts as Windows renders them, everything fits at the default and the
// minimum size.
TEST_F(EditorTest, FitsTheDefaultAndMinimumSizes)
{
    const ScopedDpiAwareness awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(editor->attached(window, kPlatformTypeHWND), kResultTrue);
    WebView& view = *editor->webView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(view); }, kPageTimeout));
    pumpUntil([] { return false; }, std::chrono::milliseconds(300)); // Fonts and layout settle.
    EXPECT_EQ(evaluate(view, kOverflowScript), "") << "at the default size";

    ViewRect minimum(0, 0, 0, 0);
    editor->checkSizeConstraint(&minimum);
    ASSERT_EQ(editor->onSize(&minimum), kResultTrue);
    ASSERT_TRUE(pumpUntil(
        [&] {
            return evaluate(view, "String(innerWidth)") == std::to_string(Editor::kMinimumWidth);
        },
        std::chrono::seconds(5)));
    pumpUntil([] { return false; }, std::chrono::milliseconds(300));
    EXPECT_EQ(evaluate(view, kOverflowScript), "") << "at the minimum size";
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
    ASSERT_EQ(editor->attached(window, kPlatformTypeHWND), kResultTrue);
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
TEST_F(EditorTest, ExplainsWhenWebView2CannotStart)
{
    SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", L"C:\\O-Scope-no-such-folder");
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(editor->attached(window, kPlatformTypeHWND), kResultTrue);
    HWND container = FindWindowExW(window, nullptr, L"OrizonOScopeWebViewHost", nullptr);
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
    SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", nullptr);
}

TEST_F(EditorTest, ExplainsWhenTheEngineIsUnavailable)
{
    HWND window = makeWindow();
    std::vector<std::string> received;
    auto editor = makeEditor(received, nullptr);
    ASSERT_EQ(editor->attached(window, kPlatformTypeHWND), kResultTrue);
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
        ASSERT_EQ(editor->attached(window, kPlatformTypeHWND), kResultTrue);
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
    // WebView2 leaves a few kernel handles behind per open: about 1 with time between opens,
    // 2.5 to 3 here, where half the editors close while WebView2 starts. Memory stays flat and
    // its processes exit; none of our calls leaves any on its own. Guards against more.
    EXPECT_LE(static_cast<int>(handlesAtEnd) - static_cast<int>(handlesAtStart), 4 * (kCycles - 2))
        << "handles left behind by " << kCycles - 2 << " open/close cycles";
}

} // namespace
} // namespace orizon::editor
