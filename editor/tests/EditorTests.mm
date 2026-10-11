#include "FitStates.h"
#include "editor/Editor.h"
#include "engine/Version.h"
#include "mac/WebViewMacTesting.h"
#include "pluginterfaces/gui/iplugview.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

// Stands in for the host's view hierarchy and records keystrokes forwarded to it.
@interface OrizonTestKeyRecorder : NSView {
    int keyDowns;
}
- (int)keyDownCount;
@end

@implementation OrizonTestKeyRecorder
- (BOOL)acceptsFirstResponder
{
    return YES;
}
- (void)keyDown:(NSEvent*)event
{
    (void)event;
    ++keyDowns;
}
- (int)keyDownCount
{
    return keyDowns;
}
@end

namespace orizon::editor {
namespace {

using namespace Steinberg;

constexpr std::chrono::seconds kPageTimeout{15};

bool pumpUntil(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
    }
    return condition();
}

class EditorTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { [NSApplication sharedApplication]; }

    NSWindow* makeWindow(NSView* content)
    {
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, Editor::kDefaultWidth, Editor::kDefaultHeight)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.releasedWhenClosed = NO;
        window.contentView = content;
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
    static std::string evaluate(WKWebView* webView, NSString* script)
    {
        struct Result
        {
            bool done = false;
            std::string value;
        };
        auto result = std::make_shared<Result>();
        [webView evaluateJavaScript:script
                  completionHandler:^(id value, NSError* /*error*/) {
                    if ([value isKindOfClass:[NSString class]]) {
                        result->value = [value UTF8String];
                    }
                    result->done = true;
                  }];
        pumpUntil([&] { return result->done; }, std::chrono::seconds(5));
        return result->value;
    }

    static bool hasSnapshot(WKWebView* webView)
    {
        return evaluate(webView, @"String(window.orizonDebug?.snapshot?.type ?? '')") == "snapshot";
    }

    // Whether any raw page message has the given type.
    static bool saw(const std::vector<std::string>& messages, const std::string& type)
    {
        return std::ranges::any_of(messages, [&](const std::string& message) {
            return message.find(R"("type":")" + type + "\"") != std::string::npos;
        });
    }
};

TEST_F(EditorTest, RunsTheWorkerOnlyWhileAttachedWithAnEngine)
{
    auto engine = std::make_shared<engine::Engine>();
    for (int cycle = 0; cycle < 10; ++cycle) {
        @autoreleasepool {
            NSView* parent = [[NSView alloc] init];
            NSWindow* window = makeWindow(parent);
            auto editor = owned(new Editor(engine));
            EXPECT_FALSE(editor->workerRunning()) << "no worker before the editor opens";
            ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
            EXPECT_TRUE(editor->workerRunning());
            EXPECT_TRUE(editor->analyzerAllocated());
            EXPECT_EQ(editor->removed(), kResultOk);
            EXPECT_FALSE(editor->workerRunning()) << "no worker after the editor closes";
            EXPECT_FALSE(editor->analyzerAllocated()) << "hold buffer released on close";
            [window close];
        }
    }
}

TEST_F(EditorTest, RunsNoWorkerWithoutAnEngine)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    auto editor = owned(new Editor(nullptr));
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    EXPECT_FALSE(editor->workerRunning());
    // The page is told why it gets no data, so it can show the unavailable state.
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    EXPECT_TRUE(pumpUntil(
        [&] {
            return evaluate(webView, @"String(window.orizonDebug?.error?.code ?? '')") ==
                   "engineUnavailable";
        },
        kPageTimeout));
    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

// Frames flow while the page asks for them, and host state changes reach the page.
// Test windows are occluded (an unbundled test process never becomes a foreground app), so
// WebKit pauses requestAnimationFrame here; the test asks for frames itself, through the page's
// real frame requests.
TEST_F(EditorTest, StreamsFramesAndReportsHostStateChanges)
{
    auto engine = std::make_shared<engine::Engine>();
    engine->setSampleRate(48000.0);
    engine->settings().set(protocol::SettingId::kTriggerMode, 2.0); // Free: a frame per request.
    std::vector<float> tone(96000);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = (i / 50) % 2 == 0 ? 0.5f : -0.5f;
    }
    std::array<const float*, 2> channels{tone.data(), tone.data()};
    engine->captureRing().write(channels.data(), 2, static_cast<int>(tone.size()));

    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    std::vector<std::string> received;
    auto editor = makeEditor(received, engine);
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();

    const auto frameCount = [&] {
        return std::stoi("0" + evaluate(webView, @"String(window.orizonDebug?.frameCount ?? 0)"));
    };
    ASSERT_TRUE(pumpUntil([&] { return frameCount() == 1; }, kPageTimeout))
        << "the page asks for its first frame on start";
    pumpUntil([] { return false; }, std::chrono::milliseconds(200));
    EXPECT_EQ(frameCount(), 1) << "no second frame without a request";

    for (int expected = 2; expected <= 5; ++expected) {
        engine->captureRing().write(channels.data(), 2, 480); // New audio, as a host would.
        evaluate(webView, @"window.orizonDebug.requestFrame(); ''");
        ASSERT_TRUE(pumpUntil([&] { return frameCount() == expected; }, std::chrono::seconds(5)))
            << "one frame per request";
    }
    EXPECT_EQ(evaluate(webView, @"String(window.orizonDebug.lastFrame.views.size)"), "2");

    engine::Settings restored;
    restored.set(protocol::SettingId::kTriggerLevel, 0.5);
    engine->settings().replace(restored);
    EXPECT_TRUE(pumpUntil(
        [&] {
            return evaluate(webView, @"String(window.orizonDebug?.lastState?.name ?? '')") ==
                   "triggerLevel";
        },
        std::chrono::seconds(5)))
        << "a host setState change should reach the page";

    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

// With the embedded fonts as WebKit renders them, everything fits at the minimum size, in
// every state that changes the labels' widths.
TEST_F(EditorTest, FitsTheMinimumSizeInEveryState)
{
    auto engine = std::make_shared<engine::Engine>();
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    std::vector<std::string> received;
    auto editor = makeEditor(received, engine);
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));
    ViewRect minimum(0, 0, 0, 0);
    editor->checkSizeConstraint(&minimum);
    ASSERT_EQ(editor->onSize(&minimum), kResultTrue);
    ASSERT_TRUE(pumpUntil(
        [&] {
            return evaluate(webView, @"String(innerWidth)") ==
                   std::to_string(Editor::kMinimumWidth);
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
            evaluate(webView, [NSString stringWithUTF8String:std::string(state.script).c_str()]);
            pumpUntil([] { return false; }, std::chrono::milliseconds(200));
        }
        EXPECT_EQ(evaluate(webView, @(fit::kOverflowScript)), "") << state.name;
    }
    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

TEST_F(EditorTest, SupportsOnlyNSView)
{
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    EXPECT_EQ(editor->isPlatformTypeSupported(kPlatformTypeNSView), kResultTrue);
    EXPECT_EQ(editor->isPlatformTypeSupported(kPlatformTypeHWND), kResultFalse);
}

TEST_F(EditorTest, EnforcesMinimumSize)
{
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ViewRect rect(0, 0, 100, 100);
    EXPECT_EQ(editor->checkSizeConstraint(&rect), kResultTrue);
    EXPECT_EQ(rect.getWidth(), Editor::kMinimumWidth);
    EXPECT_EQ(rect.getHeight(), Editor::kMinimumHeight);
}

// Stands in for the host's window: records the sizes the editor asks for, and grants them, by
// calling onSize as the VST3 contract has it or, like some hosts, without.
class TestFrame final : public IPlugFrame
{
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid) ||
            FUnknownPrivate::iidEqual(iid, FUnknown::iid)) {
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
        return callsOnSize ? view->onSize(newSize) : kResultTrue;
    }

    ViewRect requested;
    int requests = 0;
    bool callsOnSize = true;
};

// With stacked lanes on show, the minimum height grows so that every lane fits its label, and the
// editor asks the host for the room once per change.
TEST_F(EditorTest, GrowsTheMinimumHeightWithStackedLanes)
{
    for (const bool callsOnSize : {true, false}) {
        SCOPED_TRACE(callsOnSize ? "host calls onSize" : "host resizes without onSize");
        auto own = std::make_shared<engine::Engine>();
        own->settings().set(protocol::SettingId::kDisplayMode,
                            static_cast<double>(protocol::DisplayMode::kMulti));
        own->settings().set(protocol::SettingId::kViewLayout,
                            static_cast<double>(protocol::ViewLayout::kStacked));
        // Sixteen tracks: more than the default window's height fits.
        std::vector<std::shared_ptr<engine::Engine>> group{own};
        for (int track = 1; track < 16; ++track) {
            group.push_back(std::make_shared<engine::Engine>());
        }
        for (const auto& engine : group) {
            engine::InstanceRegistry::shared().add(engine);
        }
        TestFrame frame; // Outlives the editor, which keeps a pointer to it.
        frame.callsOnSize = callsOnSize;
        std::vector<std::string> received;
        auto editor = makeEditor(received, own);
        editor->setFrame(&frame);
        NSView* content = [[OrizonTestKeyRecorder alloc] initWithFrame:NSMakeRect(0, 0, 900, 500)];
        NSWindow* window = makeWindow(content);
        ASSERT_EQ(editor->attached((__bridge void*)content, kPlatformTypeNSView), kResultTrue);

        const int wanted = Editor::kChromeHeight + 16 * Editor::kMinimumLaneHeight;
        ASSERT_TRUE(pumpUntil([&] { return frame.requests > 0; }, kPageTimeout))
            << "the editor asks for the height its sixteen lanes need";
        EXPECT_EQ(frame.requested.getHeight(), wanted);
        EXPECT_EQ(editor->getRect().getHeight(), wanted) << "granted";
        auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
        EXPECT_EQ(webView.frame.size.height, wanted) << "the page has the room";
        ViewRect rect(0, 0, 100, 100);
        EXPECT_EQ(editor->checkSizeConstraint(&rect), kResultTrue);
        EXPECT_EQ(rect.getHeight(), wanted);

        own->settings().set(protocol::SettingId::kViewLayout,
                            static_cast<double>(protocol::ViewLayout::kOverlaid));
        ASSERT_TRUE(pumpUntil([&] { return editor->minimumHeight() == Editor::kMinimumHeight; },
                              kPageTimeout))
            << "overlaid lanes share the screen";
        EXPECT_EQ(frame.requests, 1) << "no request to shrink";

        editor->removed();
        [window close];
        for (const auto& engine : group) {
            engine::InstanceRegistry::shared().remove(*engine);
        }
    }
}

// The page loads from the embedded UI and completes the session handshake across
// 50 open/close cycles.
TEST_F(EditorTest, RoundTripsMessagesAcrossOpenCloseCycles)
{
    constexpr int kCycles = 50;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        @autoreleasepool {
            NSView* parent = [[NSView alloc] init];
            NSWindow* window = makeWindow(parent);
            std::vector<std::string> received;
            auto editor = makeEditor(received);

            ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
            auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
            ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout))
                << "cycle " << cycle << ": the page did not receive the session snapshot";
            EXPECT_TRUE(saw(received, "hello"));
            EXPECT_EQ(editor->removed(), kResultOk);
            EXPECT_EQ(editor->webView(), nullptr);
            EXPECT_EQ(parent.subviews.count, 0u);
            [window close];
        }
    }
}

// macOS ends a page's process under memory pressure: the page comes back on its own.
TEST_F(EditorTest, ReloadsAfterThePageProcessEnds)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));
    EXPECT_FALSE(webView.hidden);
    const auto hellos = [&] {
        return std::ranges::count_if(received, [](const std::string& message) {
            return message.find(R"("type":"hello")") != std::string::npos;
        });
    };
    ASSERT_EQ(hellos(), 1);
    ASSERT_TRUE(terminateContentProcessForTests(*editor->webView()));
    EXPECT_TRUE(pumpUntil([&] { return hellos() == 2 && hasSnapshot(webView); }, kPageTimeout))
        << "the page did not come back";
    EXPECT_FALSE(webView.hidden);
    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

// A link or a dropped file must not replace the UI.
TEST_F(EditorTest, StaysOnTheEmbeddedUi)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));
    evaluate(webView, @"location.href = 'https://example.com/'; ''");
    [webView loadRequest:[NSURLRequest requestWithURL:[NSURL fileURLWithPath:@"/tmp/page.html"]]];
    pumpUntil([] { return false; }, std::chrono::milliseconds(500));
    EXPECT_STREQ(webView.URL.absoluteString.UTF8String, "orizon://ui/index.html");
    EXPECT_TRUE(hasSnapshot(webView)) << "still the UI";
    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

TEST_F(EditorTest, ForwardsKeysToHostUnlessEditableElementHasFocus)
{
    OrizonTestKeyRecorder* host = [[OrizonTestKeyRecorder alloc] init];
    NSWindow* window = makeWindow(host);
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(editor->attached((__bridge void*)host, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));
    evaluate(webView, @"(() => { const input = document.createElement('input');"
                      @" input.id = 'test-input'; document.body.append(input); return ''; })()");
    NSEvent* key = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                    location:NSZeroPoint
                               modifierFlags:0
                                   timestamp:0
                                windowNumber:window.windowNumber
                                     context:nil
                                  characters:@"a"
                 charactersIgnoringModifiers:@"a"
                                   isARepeat:NO
                                     keyCode:0];

    NSString* expectedPrefix = [NSString
        stringWithFormat:@"OrizonOscilloscope_%@_WebView_",
                         [@(engine::kVersionString) stringByReplacingOccurrencesOfString:@"."
                                                                              withString:@"_"]];
    EXPECT_TRUE([NSStringFromClass([webView class]) hasPrefix:expectedPrefix])
        << "class name must include product and build version";

    [webView keyDown:key];
    EXPECT_EQ([host keyDownCount], 1) << "key should reach the host while nothing is focused";

    [webView evaluateJavaScript:@"document.getElementById('test-input').focus()"
              completionHandler:nil];
    pumpUntil([] { return false; }, std::chrono::seconds(1));
    [webView keyDown:key];
    EXPECT_EQ([host keyDownCount], 1) << "key should stay in the page while an input is focused";

    [webView evaluateJavaScript:@"document.getElementById('test-input').blur()"
              completionHandler:nil];
    pumpUntil([] { return false; }, std::chrono::seconds(1));
    [webView keyDown:key];
    EXPECT_EQ([host keyDownCount], 2) << "key should reach the host again after blur";

    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

} // namespace
} // namespace orizon::editor
