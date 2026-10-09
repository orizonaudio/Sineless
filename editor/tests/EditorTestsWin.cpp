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
#include <functional>
#include <gtest/gtest.h>
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

    static HWND makeWindow()
    {
        return CreateWindowExW(0, L"OrizonEditorTest", L"O-Scope test", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, Editor::kDefaultWidth,
                               Editor::kDefaultHeight, nullptr, nullptr, GetModuleHandleW(nullptr),
                               nullptr);
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
    for (int cycle = 0; cycle < kCycles; ++cycle) {
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
}

} // namespace
} // namespace orizon::editor
