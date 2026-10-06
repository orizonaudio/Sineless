#include "editor/Editor.h"
#include "engine/Version.h"
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
    // The page is told why it gets no data, so it can show the unavailable state (AD-10).
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

// Frames flow while credits return, and host state changes reach the page (AD-12, DESIGN 6.1).
// Test windows are occluded (an unbundled test process never becomes a foreground app), so
// WebKit pauses requestAnimationFrame here; the test returns credits itself, through the page's
// real native channel. rAF pacing in a host was measured in T-006.
TEST_F(EditorTest, StreamsFramesAndReportsHostStateChanges)
{
    auto engine = std::make_shared<engine::Engine>();
    engine->setSampleRate(48000.0);
    engine->settings().set(protocol::SettingId::kTriggerMode, 2.0); // Free: a frame per credit.
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
        << "the first frame needs no credit";
    pumpUntil([] { return false; }, std::chrono::milliseconds(200));
    EXPECT_EQ(frameCount(), 1) << "no second frame without a credit";

    for (int expected = 2; expected <= 5; ++expected) {
        engine->captureRing().write(channels.data(), 2, 480); // New audio, as a host would.
        evaluate(webView, @"window.webkit.messageHandlers.orizon.postMessage("
                          @"JSON.stringify({type: 'credit'})); ''");
        ASSERT_TRUE(pumpUntil([&] { return frameCount() == expected; }, std::chrono::seconds(5)))
            << "one frame per credit";
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

// The page loads from the embedded UI and completes the session handshake (DESIGN 6.1) across
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
        << "class name must include product and build version (AD-4)";

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
