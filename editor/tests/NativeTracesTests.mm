#include "MacPageTesting.h"
#include "editor/Editor.h"
#include "editor/EditorCore.h"
#include "mac/TraceLayerMac.h"
#include "mac/WebViewMacTesting.h"
#include "pluginterfaces/gui/iplugview.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <WebKit/WebKit.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <objc/message.h>
#include <string>
#include <vector>

namespace orizon::editor {
namespace {

using namespace Steinberg;
using namespace std::chrono_literals;
using testing::evaluate;
using testing::hasSnapshot;
using testing::kPageTimeout;
using testing::pumpUntil;

// The page's trace canvas: its box in CSS pixels from the top left, and its size in device pixels.
struct PageScreen
{
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;
    double canvasWidth = 0.0;
    double canvasHeight = 0.0;
};

PageScreen pageScreen(WKWebView* webView)
{
    const std::string text = evaluate(
        webView, @"(() => { const c = document.getElementById('trace'); "
                 @"const r = c.getBoundingClientRect(); "
                 @"return JSON.stringify([r.left, r.top, r.width, r.height, c.width, c.height]); "
                 @"})()");
    if (text.empty()) {
        return {};
    }
    const auto values = nlohmann::json::parse(text);
    return {.left = values[0],
            .top = values[1],
            .width = values[2],
            .height = values[3],
            .canvasWidth = values[4],
            .canvasHeight = values[5]};
}

class NativeTracesTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { [NSApplication sharedApplication]; }

    void SetUp() override
    {
        // On screen, whatever the display reports: an idle one reports every window occluded,
        // though the display link still runs.
        TraceLayer::setOcclusionForTesting(false);
        engine = std::make_shared<engine::Engine>();
        engine->prepare(48000.0);
        engine->settings().set(protocol::SettingId::kTimeBase, 0.0);    // Seconds.
        engine->settings().set(protocol::SettingId::kTriggerMode, 2.0); // Free: always moving.
        feed();
    }

    void TearDown() override
    {
        close();
        TraceLayer::setOcclusionForTesting(std::nullopt);
    }

    // Runs the main run loop until `condition` holds, delivering audio as a host would meanwhile,
    // so the trace keeps changing and frames keep being made.
    bool pumpPlaying(const std::function<bool()>& condition, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition() && std::chrono::steady_clock::now() < deadline) {
            feed(480);
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        }
        return condition();
    }

    // A square wave, as a host would deliver it.
    void feed(std::size_t samples = 48000)
    {
        std::vector<float> tone(samples);
        for (std::size_t i = 0; i < tone.size(); ++i) {
            tone[i] = (i / 50) % 2 == 0 ? 0.5f : -0.5f;
        }
        std::array<const float*, 2> channels{tone.data(), tone.data()};
        engine->captureRing().write(channels.data(), 2, static_cast<int>(tone.size()));
    }

    void open()
    {
        parent = [[NSView alloc] init];
        window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(100, 100, Editor::kDefaultWidth, Editor::kDefaultHeight)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.releasedWhenClosed = NO;
        window.contentView = parent;
        [window orderFrontRegardless];
        editor = owned(new Editor(
            engine, [this](std::string_view message) { received.emplace_back(message); }));
        ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
        webView = (__bridge WKWebView*)editor->webView()->nativeView();
        // The page visible whatever the display reports, as WebKit's own tests make it: an idle
        // display reports every window occluded, and WebKit then runs no animation frames.
        const SEL occlusion = NSSelectorFromString(@"_setWindowOcclusionDetectionEnabled:");
        if ([webView respondsToSelector:occlusion]) {
            reinterpret_cast<void (*)(id, SEL, BOOL)>(objc_msgSend)(webView, occlusion, NO);
        }
        ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));
        layer = traceLayerForTests(*editor->webView());
    }

    void close()
    {
        if (editor) {
            EXPECT_EQ(editor->removed(), kResultOk);
            editor = nullptr;
        }
        [window orderOut:nil];
        [window close];
        window = nil;
        layer = nullptr;
    }

    // A new frame reaches the screen. Audio keeps coming meanwhile: with none, nothing new is
    // drawn.
    ::testing::AssertionResult presents(std::chrono::milliseconds within = 5s)
    {
        const auto before = layer->statsForTesting().submitted;
        if (pumpPlaying([&] { return layer->statsForTesting().submitted > before; }, within)) {
            return ::testing::AssertionSuccess();
        }
        const auto stats = layer->statsForTesting();
        return ::testing::AssertionFailure()
               << "nothing presented: " << stats.callbacks << " callbacks, " << stats.submitted
               << " submitted, hidden " << (layer->view().hidden ? "yes" : "no") << ", in a window "
               << (layer->view().window != nil ? "yes" : "no") << ", page frames " << frameCount();
    }

    int frameCount()
    {
        return std::stoi("0" + evaluate(webView, @"String(window.orizonDebug?.frameCount ?? 0)"));
    }

    // The layer sits exactly on the page's screen, its drawable the page's canvas size.
    bool coversThePagesScreen()
    {
        const PageScreen page = pageScreen(webView);
        NSView* container = webView.superview;
        const NSRect frame = layer->view().frame;
        const double height = container.bounds.size.height;
        const auto* metal = static_cast<CAMetalLayer*>(layer->view().layer);
        return frame.origin.x == page.left && frame.size.width == page.width &&
               frame.size.height == page.height &&
               frame.origin.y == height - page.top - page.height &&
               metal.drawableSize.width == page.canvasWidth &&
               metal.drawableSize.height == page.canvasHeight;
    }

    // Waits for the layer to cover the page's screen; if it does not, says where each is.
    ::testing::AssertionResult coversThePagesScreenWithin(std::chrono::milliseconds within)
    {
        if (pumpUntil([&] { return coversThePagesScreen(); }, within)) {
            return ::testing::AssertionSuccess();
        }
        const PageScreen page = pageScreen(webView);
        const NSRect frame = layer->view().frame;
        const auto* metal = static_cast<CAMetalLayer*>(layer->view().layer);
        return ::testing::AssertionFailure()
               << "page screen " << page.left << ", " << page.top << ", " << page.width << " × "
               << page.height << " (canvas " << page.canvasWidth << " × " << page.canvasHeight
               << "); layer " << frame.origin.x << ", " << frame.origin.y << ", "
               << frame.size.width << " × " << frame.size.height << " (drawable "
               << metal.drawableSize.width << " × " << metal.drawableSize.height << "); container "
               << webView.superview.bounds.size.width << " × "
               << webView.superview.bounds.size.height << ", web view " << webView.frame.size.width
               << " × " << webView.frame.size.height << ", scale " << window.backingScaleFactor
               << ", page ratio " << evaluate(webView, @"String(window.devicePixelRatio)");
    }

    std::shared_ptr<engine::Engine> engine;
    NSView* parent = nil;
    NSWindow* window = nil;
    IPtr<Editor> editor;
    WKWebView* webView = nil;
    TraceLayer* layer = nullptr;
    std::vector<std::string> received;
};

// The editor draws the traces below a transparent page, at the page's screen, and the page knows
// it: it creates no WebGL context and draws no grid.
TEST_F(NativeTracesTest, DrawsTheTracesBelowATransparentPage)
{
    open();
    ASSERT_NE(layer, nullptr);
    EXPECT_EQ(evaluate(webView, @"String(document.documentElement.classList.contains("
                                @"'native-traces'))"),
              "true");
    EXPECT_EQ(evaluate(webView, @"String(window.orizonDebug.snapshot.nativeTraces)"), "true");
    EXPECT_FALSE([[webView valueForKey:@"drawsBackground"] boolValue]);
    NSArray<NSView*>* stack = webView.superview.subviews;
    EXPECT_LT([stack indexOfObject:layer->view()], [stack indexOfObject:webView]);
    ASSERT_TRUE(presents());
    EXPECT_FALSE(layer->view().hidden);
    EXPECT_TRUE(coversThePagesScreenWithin(2s));
}

// Around the screen, where the page is transparent too, the editor shows the page's colour.
TEST_F(NativeTracesTest, ShowsThePageColourAroundTheTraces)
{
    open();
    const CGColorRef native = webView.superview.layer.backgroundColor;
    ASSERT_NE(native, nullptr);
    NSColor* colour =
        [[NSColor colorWithCGColor:native] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    const std::string expected = "rgb(" + std::to_string(std::lround(colour.redComponent * 255)) +
                                 ", " + std::to_string(std::lround(colour.greenComponent * 255)) +
                                 ", " + std::to_string(std::lround(colour.blueComponent * 255)) +
                                 ")";
    EXPECT_EQ(evaluate(webView, @"(() => { const e = document.createElement('div'); "
                                @"e.style.backgroundColor = 'var(--sl-case)'; "
                                @"document.body.append(e); "
                                @"const c = getComputedStyle(e).backgroundColor; e.remove(); "
                                @"return c; })()"),
              expected);
    EXPECT_EQ(evaluate(webView, @"getComputedStyle(document.body).backgroundColor"),
              "rgba(0, 0, 0, 0)");
}

// Through every size the host gives, the traces stay on the page's screen and are drawn at its
// size: the minimum, larger, narrower than the minimum (the page keeps its layout and is clipped),
// and back; shorter than the toolbar and status bar, they hide.
TEST_F(NativeTracesTest, StaysOnThePagesScreenThroughEverySize)
{
    open();
    ASSERT_TRUE(presents());
    for (const auto& [width, height] :
         std::vector<std::array<int, 2>>{{Editor::kMinimumWidth, Editor::kMinimumHeight},
                                         {1500, 900},
                                         {900, 500},
                                         {Editor::kDefaultWidth, Editor::kDefaultHeight}}) {
        ViewRect rect(0, 0, width, height);
        ASSERT_EQ(editor->onSize(&rect), kResultTrue);
        EXPECT_TRUE(coversThePagesScreenWithin(3s)) << width << " × " << height;
        const auto* metal = static_cast<CAMetalLayer*>(layer->view().layer);
        EXPECT_TRUE(pumpUntil(
            [&] {
                const auto stats = layer->statsForTesting();
                return stats.drawnWidth == metal.drawableSize.width &&
                       stats.drawnHeight == metal.drawableSize.height;
            },
            3s))
            << width << " × " << height;
        EXPECT_FALSE(layer->view().hidden);
    }
    ViewRect low(0, 0, Editor::kDefaultWidth, protocol::kToolbarHeight);
    editor->onSize(&low);
    EXPECT_TRUE(layer->view().hidden);
}

// The page's readouts, hover and status bar read copies of the frames drawn.
TEST_F(NativeTracesTest, GivesThePageCopiesOfTheFramesDrawn)
{
    open();
    ASSERT_TRUE(presents());
    const int before = frameCount();
    EXPECT_TRUE(pumpPlaying([&] { return frameCount() > before + 3; }, 3s));
    EXPECT_EQ(evaluate(webView, @"String(window.orizonDebug.lastFrame.views.size)"), "2");
}

// While the page reloads after its process ended, the traces hide; the reloaded page's trace view
// shows them again.
TEST_F(NativeTracesTest, HidesTheTracesWhileThePageReloads)
{
    open();
    ASSERT_TRUE(presents());
    ASSERT_TRUE(terminateContentProcessForTests(*editor->webView()));
    EXPECT_TRUE(pumpUntil([&] { return layer->view().hidden; }, 5s));
    EXPECT_TRUE(
        pumpUntil([&] { return hasSnapshot(webView) && !layer->view().hidden; }, kPageTimeout));
    EXPECT_TRUE(presents());
}

// A page that keeps dying is not reloaded again: the editor's notice says so, and the last traces
// drawn do not stay behind it.
TEST_F(NativeTracesTest, HidesTheTracesBehindTheNoticeThatThePageKeepsFailing)
{
    open();
    for (int reload = 0; reload < 3; ++reload) {
        ASSERT_TRUE(
            pumpUntil([&] { return hasSnapshot(webView) && !layer->view().hidden; }, kPageTimeout))
            << "reload " << reload;
        ASSERT_TRUE(terminateContentProcessForTests(*editor->webView()));
        pumpUntil([&] { return !hasSnapshot(webView); }, 2s);
    }
    ASSERT_TRUE(
        pumpUntil([&] { return hasSnapshot(webView) && !layer->view().hidden; }, kPageTimeout));
    ASSERT_TRUE(terminateContentProcessForTests(*editor->webView()));
    ASSERT_TRUE(pumpUntil([&] { return webView.hidden; }, 5s)) << "the notice is shown";
    pumpUntil([] { return false; }, 1s);
    EXPECT_TRUE(layer->view().hidden);
}

// Drawn natively, the analyzer places its frames and sweeps by the display clock, which steps by
// whole refreshes (AD-30): by the host's time instead, the traces would move unevenly.
TEST_F(NativeTracesTest, PlacesTheFramesByTheDisplayClock)
{
    open();
    ASSERT_TRUE(presents());
    const engine::FramePacing* clock = editor->displayClock();
    ASSERT_NE(clock, nullptr);
    const auto before = clock->clockReads();
    const int frames = frameCount();
    EXPECT_TRUE(
        pumpPlaying([&] { return frameCount() > frames + 3 && clock->clockReads() > before; }, 5s));
}

// The diagnostic switch is the host's environment, read as the editor opens: set, the page is told
// and the layer numbers every picture; unset, neither.
TEST_F(NativeTracesTest, DrawsTheFrameBarcodeOnlyWhenTheHostSetsTheSwitch)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe): the test's own thread, before the editor opens.
    setenv("SINELESS_FRAME_BARCODE", "1", 1);
    open();
    // NOLINTNEXTLINE(concurrency-mt-unsafe): as above.
    unsetenv("SINELESS_FRAME_BARCODE");
    ASSERT_TRUE(presents());
    EXPECT_EQ(evaluate(webView, @"String(window.orizonDebug.snapshot.frameBarcode)"), "true");
    EXPECT_TRUE(layer->statsForTesting().barcode);
    close();

    open();
    ASSERT_TRUE(presents());
    EXPECT_EQ(evaluate(webView, @"String(window.orizonDebug.snapshot.frameBarcode)"), "undefined");
    EXPECT_FALSE(layer->statsForTesting().barcode);
}

// Once native drawing stops for good, the page says what to do, and its own fetch makes the frames
// again, so its readouts stay live.
TEST_F(NativeTracesTest, SaysWhatToDoWhenNativeDrawingStops)
{
    open();
    ASSERT_TRUE(presents());
    layer->failCommandBuffersForTesting(11); // MTLCommandBufferErrorDeviceRemoved.
    EXPECT_TRUE(pumpUntil(
        [&] {
            return evaluate(webView,
                            @"String(!document.getElementById('message').hidden && "
                            @"document.getElementById('message-text').textContent.includes("
                            @"'reopen'))") == "true";
        },
        5s));
    const int before = frameCount();
    EXPECT_TRUE(pumpPlaying([&] { return frameCount() > before + 3; }, 3s));
}

// Opened and closed again and again: each time the traces are drawn natively and nothing is left
// running.
TEST_F(NativeTracesTest, OpensAndClosesCleanly)
{
    for (int cycle = 0; cycle < 4; ++cycle) {
        @autoreleasepool {
            open();
            ASSERT_NE(layer, nullptr);
            EXPECT_TRUE(presents()) << cycle;
            close();
            EXPECT_FALSE(editor);
        }
    }
}

// Turned off (the showcase), the page draws the traces itself.
TEST_F(NativeTracesTest, LeavesTheTracesToThePageWhenTurnedOff)
{
    EditorCore::setNativeTraces(false);
    open();
    EditorCore::setNativeTraces(true);
    EXPECT_EQ(layer, nullptr);
    EXPECT_EQ(evaluate(webView, @"String(document.documentElement.classList.contains("
                                @"'native-traces'))"),
              "false");
    EXPECT_EQ(evaluate(webView, @"String(window.orizonDebug.snapshot.nativeTraces)"), "undefined");
}

} // namespace
} // namespace orizon::editor
