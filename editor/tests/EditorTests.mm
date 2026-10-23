#include "FitStates.h"
#include "MacPageTesting.h"
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
using testing::evaluate;
using testing::hasSnapshot;
using testing::kPageTimeout;
using testing::pumpUntil;
using testing::saw;

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

// The page runs only the script it was built with: a script put into it some other way (a host
// track name that ever reached markup, say) is refused.
TEST_F(EditorTest, RunsNoScriptThePageWasNotBuiltWith)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    auto editor = owned(new Editor(nullptr));
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    ASSERT_TRUE(pumpUntil(
        [&] { return evaluate(webView, @"String(window.orizonDebug !== undefined)") == "true"; },
        kPageTimeout));
    EXPECT_EQ(evaluate(webView, @"(() => {"
                                 "  const script = document.createElement('script');"
                                 "  script.textContent = 'window.injected = true';"
                                 "  document.head.append(script);"
                                 "  return String(window.injected === true);"
                                 "})()"),
              "false");
    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

// What the page may load lets in everything it was built with: its stylesheet applies, and every
// font it declares loads.
TEST_F(EditorTest, LoadsItsOwnStylesAndFonts)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    auto editor = owned(new Editor(nullptr));
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    ASSERT_TRUE(pumpUntil(
        [&] { return evaluate(webView, @"String(window.orizonDebug !== undefined)") == "true"; },
        kPageTimeout));
    evaluate(webView, @"(document.styleSheets.length > 0 && document.fonts.size > 0"
                       "   ? Promise.all([...document.fonts].map((font) => font.load()))"
                       "   : Promise.reject()).then("
                       "  () => { window.fonts = 'loaded'; }, () => { window.fonts = 'failed'; });"
                       "''");
    EXPECT_TRUE(
        pumpUntil([&] { return evaluate(webView, @"String(window.fonts ?? '')") == "loaded"; },
                  kPageTimeout));
    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

// The view shows the page's cursor itself: WebKit does not when the view is hosted out of process
// (an Audio Unit in Logic). A pointer over each kind of control the page has, and over nothing
// in particular, gives that control's cursor.
TEST_F(EditorTest, ShowsThePagesCursorForWhatIsUnderThePointer)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    auto editor = owned(new Editor(nullptr));
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    WebView& view = *editor->webView();
    auto* webView = (__bridge WKWebView*)view.nativeView();
    ASSERT_TRUE(pumpUntil(
        [&] { return evaluate(webView, @"String(window.orizonDebug !== undefined)") == "true"; },
        kPageTimeout));
    std::vector<std::pair<NSString*, NSCursor*>> cases = {
        {@".field", [NSCursor resizeUpDownCursor]},       // A value field, dragged up and down.
        {@"#cursor-a", [NSCursor resizeLeftRightCursor]}, // A time cursor.
        {@"#trigger-marker", [NSCursor resizeUpDownCursor]},
        {@"button", [NSCursor pointingHandCursor]},
        {@"#labels", [NSCursor arrowCursor]},
    };
    NSCursor* diagonal = (__bridge NSCursor*)drawnDiagonalResizeCursorForTests();
    if (@available(macOS 15.0, *)) {
        diagonal = [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionBottomRight
                                              inDirections:NSCursorFrameResizeDirectionsAll];
    }
    cases.emplace_back(@"#resize-grip", diagonal);
    for (const auto& [selector, expected] : cases) {
        SCOPED_TRACE(selector.UTF8String);
        [[NSCursor crosshairCursor] set]; // None the page uses: the message must set the cursor.
        evaluate(webView, [NSString stringWithFormat:@"document.querySelector('%@').dispatchEvent("
                                                      "new PointerEvent('pointerover', "
                                                      "{bubbles: true})); ''",
                                                     selector]);
        EXPECT_TRUE(pumpUntil([&] { return (__bridge NSCursor*)cursorForTests(view) == expected; },
                              std::chrono::seconds(2)));
        EXPECT_EQ([NSCursor currentCursor], expected) << "shown at once, without a mouse move";
    }

    // The test window is never key, as an out-of-process Audio Unit's is not: WebKit shows no
    // cursor, and each move or drag over the view sets the page's.
    evaluate(webView, @"document.querySelector('.field').dispatchEvent("
                       "new PointerEvent('pointerover', {bubbles: true})); ''");
    ASSERT_TRUE(pumpUntil(
        [&] { return (__bridge NSCursor*)cursorForTests(view) == [NSCursor resizeUpDownCursor]; },
        std::chrono::seconds(2)));
    for (const NSEventType type : {NSEventTypeMouseMoved, NSEventTypeLeftMouseDragged}) {
        [[NSCursor arrowCursor] set];
        NSEvent* event = [NSEvent mouseEventWithType:type
                                            location:NSMakePoint(10, 10)
                                       modifierFlags:0
                                           timestamp:0
                                        windowNumber:window.windowNumber
                                             context:nil
                                         eventNumber:0
                                          clickCount:type == NSEventTypeMouseMoved ? 0 : 1
                                            pressure:0];
        if (type == NSEventTypeMouseMoved) {
            [webView mouseMoved:event];
        } else {
            [webView mouseDragged:event];
        }
        EXPECT_EQ([NSCursor currentCursor], [NSCursor resizeUpDownCursor]) << type;
    }
    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

// Every cursor the page's stylesheet uses has a macOS cursor the view can show: one without would
// leave the previous cursor showing over that control where WebKit shows none.
TEST_F(EditorTest, HasAMacCursorForEveryCursorThePageUses)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    auto editor = owned(new Editor(nullptr));
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    ASSERT_TRUE(pumpUntil(
        [&] { return evaluate(webView, @"String(window.orizonDebug !== undefined)") == "true"; },
        kPageTimeout));
    const std::string used = evaluate(
        webView, @"[...new Set([...document.styleSheets].flatMap((sheet) => [...sheet.cssRules])"
                  "  .map((rule) => rule.style?.cursor ?? '').filter((cursor) => cursor !== ''))]"
                  "  .join(',')");
    ASSERT_FALSE(used.empty()) << "the page's stylesheet declares cursors";
    std::size_t start = 0;
    while (start <= used.size()) {
        const std::size_t end = std::min(used.find(',', start), used.size());
        const std::string cursor = used.substr(start, end - start);
        EXPECT_NE(cursorForCssForTests(cursor.c_str()), nullptr) << cursor;
        start = end + 1;
    }
    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

// Before macOS 15, which has no public diagonal resize cursor, the resize grip shows one drawn
// from the system symbol, with its hot spot at the arrows' centre. Drawn on any macOS, so the
// tests check it whatever macOS they run on.
TEST_F(EditorTest, DrawsADiagonalResizeCursorForMacOSBefore15)
{
    auto* cursor = (__bridge NSCursor*)drawnDiagonalResizeCursorForTests();
    ASSERT_NE(cursor, nil);
    const NSSize size = cursor.image.size;
    EXPECT_GT(size.width, 0.0);
    EXPECT_GT(size.height, 0.0);
    EXPECT_DOUBLE_EQ(cursor.hotSpot.x, size.width / 2);
    EXPECT_DOUBLE_EQ(cursor.hotSpot.y, size.height / 2);
}

// Frames flow while the page asks for them, and host state changes reach the page.
// Test windows are occluded (an unbundled test process never becomes a foreground app), so
// WebKit pauses requestAnimationFrame here; the test asks for frames itself, through the page's
// real frame requests.
TEST_F(EditorTest, StreamsFramesAndReportsHostStateChanges)
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
    restored.set(protocol::SettingId::kTimeBase, 0.0); // As before: only the level changes.
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
        EXPECT_LE(std::stoi(evaluate(webView, @(fit::kChromeScript))), Editor::kChromeHeight)
            << state.name;
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

// What the page sends when the user drags its resize grip; `fields` is the message's body.
void postResize(WKWebView* webView, const std::string& fields)
{
    evaluate(webView,
             [NSString stringWithFormat:@"window.webkit.messageHandlers.orizon.postMessage("
                                        @"JSON.stringify({type: 'resize', %s})); ''",
                                        fields.c_str()]);
}
void dragGrip(WKWebView* webView, int width, int height)
{
    postResize(webView, "width: " + std::to_string(width) + ", height: " + std::to_string(height));
}

// Dragging the editor's resize grip asks the host for that size, which works in hosts that offer
// no edge of their own to drag; never smaller than the editor's minimum, and nothing for a size
// that is not one.
TEST_F(EditorTest, AsksTheHostForTheSizeTheGripIsDraggedTo)
{
    for (const bool callsOnSize : {true, false}) {
        SCOPED_TRACE(callsOnSize ? "host calls onSize" : "host resizes without onSize");
        TestFrame frame; // Outlives the editor, which keeps a pointer to it.
        frame.callsOnSize = callsOnSize;
        std::vector<std::string> received;
        auto editor = makeEditor(received);
        editor->setFrame(&frame);
        NSView* content = [[OrizonTestKeyRecorder alloc]
            initWithFrame:NSMakeRect(0, 0, Editor::kDefaultWidth, Editor::kDefaultHeight)];
        NSWindow* window = makeWindow(content);
        ASSERT_EQ(editor->attached((__bridge void*)content, kPlatformTypeNSView), kResultTrue);
        auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
        ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));

        const int larger = Editor::kDefaultWidth + 300;
        const int taller = Editor::kDefaultHeight + 200;
        dragGrip(webView, larger, taller);
        ASSERT_TRUE(pumpUntil([&] { return frame.requests == 1; }, kPageTimeout));
        EXPECT_EQ(frame.requested.getWidth(), larger);
        EXPECT_EQ(frame.requested.getHeight(), taller);
        EXPECT_EQ(editor->getRect().getWidth(), larger) << "granted";
        EXPECT_TRUE(pumpUntil([&] { return webView.frame.size.height == taller; }, kPageTimeout))
            << "the page has the room";

        dragGrip(webView, 300, 200);
        ASSERT_TRUE(pumpUntil([&] { return frame.requests == 2; }, kPageTimeout));
        EXPECT_EQ(frame.requested.getWidth(), Editor::kMinimumWidth);
        EXPECT_EQ(frame.requested.getHeight(), editor->minimumHeight());

        for (const std::string fields : {"width: 'wide', height: 700", "width: 1200, height: null",
                                         "width: 1200", "height: 700", ""}) {
            postResize(webView, fields);
        }
        dragGrip(webView, larger, taller); // Handled after them: the page's order is kept.
        ASSERT_TRUE(pumpUntil([&] { return frame.requests >= 3; }, kPageTimeout));
        EXPECT_EQ(frame.requests, 3) << "no request for a size that is not one";
        EXPECT_EQ(frame.requested.getWidth(), larger);

        // Last: the page at the size granted is far larger than any display, and slow to draw.
        dragGrip(webView, 1000000, 1000000);
        ASSERT_TRUE(pumpUntil([&] { return frame.requests == 4; }, kPageTimeout));
        EXPECT_GT(frame.requested.getWidth(), larger);
        EXPECT_LT(frame.requested.getWidth(), 1000000)
            << "a size within reason, not the absurd one";
        EXPECT_LT(frame.requested.getHeight(), 1000000);

        editor->removed();
        [window close];
    }
}

// With stacked lanes on show, the grip cannot make the editor too short for every lane.
TEST_F(EditorTest, KeepsTheLanesHeightWhenTheGripIsDraggedIn)
{
    auto own = std::make_shared<engine::Engine>();
    own->settings().set(protocol::SettingId::kDisplayMode,
                        static_cast<double>(protocol::DisplayMode::kMulti));
    own->settings().set(protocol::SettingId::kViewLayout,
                        static_cast<double>(protocol::ViewLayout::kStacked));
    std::vector<std::shared_ptr<engine::Engine>> group{own};
    for (int track = 1; track < 16; ++track) {
        group.push_back(std::make_shared<engine::Engine>());
    }
    for (const auto& engine : group) {
        engine::InstanceRegistry::shared().add(engine);
    }
    TestFrame frame;
    std::vector<std::string> received;
    auto editor = makeEditor(received, own);
    editor->setFrame(&frame);
    NSView* content = [[OrizonTestKeyRecorder alloc]
        initWithFrame:NSMakeRect(0, 0, Editor::kDefaultWidth, Editor::kDefaultHeight)];
    NSWindow* window = makeWindow(content);
    ASSERT_EQ(editor->attached((__bridge void*)content, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    const int wanted = Editor::kChromeHeight + 16 * Editor::kMinimumLaneHeight;
    ASSERT_TRUE(pumpUntil([&] { return frame.requests == 1; }, kPageTimeout))
        << "the editor asks for the height its sixteen lanes need";
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));

    dragGrip(webView, Editor::kMinimumWidth, Editor::kMinimumHeight);
    ASSERT_TRUE(pumpUntil([&] { return frame.requests == 2; }, kPageTimeout));
    EXPECT_EQ(frame.requested.getHeight(), wanted);

    editor->removed();
    [window close];
    for (const auto& engine : group) {
        engine::InstanceRegistry::shared().remove(*engine);
    }
}

// The editor opens again at the size it was left at, for as long as the plugin stays loaded.
TEST_F(EditorTest, OpensAtTheSizeItWasLeftAt)
{
    auto engine = std::make_shared<engine::Engine>();
    TestFrame frame;
    std::vector<std::string> received;
    {
        auto editor = makeEditor(received, engine);
        editor->setFrame(&frame);
        NSView* content = [[OrizonTestKeyRecorder alloc]
            initWithFrame:NSMakeRect(0, 0, Editor::kDefaultWidth, Editor::kDefaultHeight)];
        NSWindow* window = makeWindow(content);
        ASSERT_EQ(editor->attached((__bridge void*)content, kPlatformTypeNSView), kResultTrue);
        auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
        ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));
        dragGrip(webView, 1300, 800);
        ASSERT_TRUE(pumpUntil([&] { return frame.requests == 1; }, kPageTimeout));
        editor->removed();
        [window close];
    }
    auto reopened = makeEditor(received, engine);
    EXPECT_EQ(reopened->getRect().getWidth(), 1300);
    EXPECT_EQ(reopened->getRect().getHeight(), 800);

    auto another = makeEditor(received);
    EXPECT_EQ(another->getRect().getWidth(), Editor::kDefaultWidth) << "another instance's own";

    // A host that left it smaller than its minimum: it opens again at the minimum, not cropped.
    ViewRect small(0, 0, 300, 200);
    reopened->onSize(&small);
    auto again = makeEditor(received, engine);
    EXPECT_EQ(again->getRect().getWidth(), Editor::kMinimumWidth);
    EXPECT_EQ(again->getRect().getHeight(), Editor::kMinimumHeight);
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
    // Pages that load at once, with no network: without the guard they would replace the UI.
    evaluate(webView, @"location.href = 'about:blank'; ''");
    pumpUntil([] { return false; }, std::chrono::milliseconds(300));
    [webView
        loadRequest:[NSURLRequest
                        requestWithURL:[NSURL URLWithString:@"data:text/html,<p>dropped</p>"]]];
    pumpUntil([] { return false; }, std::chrono::milliseconds(300));
    EXPECT_STREQ(webView.URL.absoluteString.UTF8String, "orizon://ui/index.html");
    EXPECT_TRUE(hasSnapshot(webView)) << "still the UI";
    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

// A page process that keeps dying is not reloaded forever: after three reloads in a minute the
// editor says so instead.
TEST_F(EditorTest, ExplainsWhenThePageKeepsFailing)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    for (int reload = 0; reload < 3; ++reload) {
        ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout))
            << "reload " << reload;
        ASSERT_TRUE(terminateContentProcessForTests(*editor->webView()));
        pumpUntil([&] { return !hasSnapshot(webView); }, std::chrono::seconds(2));
    }
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));
    ASSERT_TRUE(terminateContentProcessForTests(*editor->webView()));
    const auto noticeShown = [&] {
        for (NSView* view in webView.superview.subviews) {
            if ([view isKindOfClass:[NSTextField class]] && !view.hidden &&
                [static_cast<NSTextField*>(view).stringValue containsString:@"keeps failing"]) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(pumpUntil(noticeShown, std::chrono::seconds(5)));
    EXPECT_TRUE(webView.hidden);
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
