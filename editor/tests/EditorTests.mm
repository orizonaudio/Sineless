#include "Branding.h"
#include "FitStates.h"
#include "MacPageTesting.h"
#include "editor/Editor.h"
#include "editor/EditorCore.h"
#include "editor/PointerHold.h"
#include "editor/Transport.h"
#include "editor/UiDispatcher.h"
#include "engine/Version.h"
#include "mac/UiDispatcherMacTesting.h"
#include "mac/WebViewMacTesting.h"
#include "pluginterfaces/gui/iplugview.h"

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <dlfcn.h>
#include <functional>
#include <gtest/gtest.h>
#include <initializer_list>
#include <mach/mach_time.h>
#include <memory>
#include <objc/runtime.h>
#include <ostream>
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

// Counts the times one view's cursor rectangles are invalidated, which AppKit then asks for anew.
@interface OrizonTestCursorWindow : NSWindow {
    int invalidations;
    __weak NSView* watched;
}
- (void)watch:(NSView*)view;
- (int)invalidationCount;
@end

@implementation OrizonTestCursorWindow
- (void)invalidateCursorRectsForView:(NSView*)view
{
    if (view == watched) {
        ++invalidations;
    }
    [super invalidateCursorRectsForView:view];
}
- (void)watch:(NSView*)view
{
    watched = view;
}
- (int)invalidationCount
{
    return invalidations;
}
@end

namespace orizon::editor {
namespace {

// The cursor rectangles views add, as AppKit receives them.
struct CursorRect
{
    NSRect rect;
    NSCursor* cursor;
};

std::vector<CursorRect>& addedCursorRects()
{
    static std::vector<CursorRect> added;
    return added;
}

void recordCursorRect(id self, SEL cmd, NSRect rect, NSCursor* cursor)
{
    addedCursorRects().push_back({.rect = rect, .cursor = cursor});
    using Add = void (*)(id, SEL, NSRect, NSCursor*);
    reinterpret_cast<Add>(class_getMethodImplementation([NSView class], cmd))(self, cmd, rect,
                                                                              cursor);
}

// The cursor rectangles `view` gives AppKit when asked for them.
std::vector<CursorRect> cursorRectsOf(NSView* view)
{
    const SEL add = @selector(addCursorRect:cursor:);
    class_replaceMethod([view class], add, reinterpret_cast<IMP>(&recordCursorRect),
                        method_getTypeEncoding(class_getInstanceMethod([NSView class], add)));
    addedCursorRects().clear();
    [view discardCursorRects];
    [view resetCursorRects];
    return addedCursorRects();
}

using namespace Steinberg;
using testing::evaluate;
using testing::hasSnapshot;
using testing::kPageTimeout;
using testing::pumpUntil;
using testing::saw;

class EditorTest : public ::testing::Test
{
protected:
    // The page draws the traces, as on Windows: these tests cover the page's own frame flow and
    // drawing; NativeTracesTest covers the traces drawn natively.
    static void SetUpTestSuite()
    {
        [NSApplication sharedApplication];
        EditorCore::setNativeTraces(false);
    }
    static void TearDownTestSuite() { EditorCore::setNativeTraces(true); }

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

// Each editor opened starts a repeating timer on the UI thread. Closing the editor frees it: one
// left behind per editor would stay for as long as the host runs.
TEST_F(EditorTest, FreesItsTimersWhenClosed)
{
    const int before = liveTimersForTests();
    for (int opened = 0; opened < 3; ++opened) {
        auto dispatcher = UiDispatcher::create();
        dispatcher->every(std::chrono::milliseconds(10), [] {});
        pumpUntil([] { return false; }, std::chrono::milliseconds(30)); // It runs.
        EXPECT_EQ(liveTimersForTests(), before + 1);
    }
    EXPECT_TRUE(pumpUntil([&] { return liveTimersForTests() == before; }, std::chrono::seconds(2)))
        << liveTimersForTests() - before << " timers left";
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

// The page keeps nothing it needs in WebKit's storage (settings live in the engine), so it leaves
// nothing of its own on disk, in the host's data folder.
TEST_F(EditorTest, KeepsNoWebsiteDataOnDisk)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    auto editor = owned(new Editor(nullptr));
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    EXPECT_FALSE(webView.configuration.websiteDataStore.persistent);
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

// The VST3 editor leaves the page's cursors to WebKit, as in every VST3 host: over a control with
// its own cursor, the view gives AppKit no cursor rectangle and shows no cursor of its own when
// AppKit asks.
TEST_F(EditorTest, LeavesTheCursorsToWebKit)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    auto editor = owned(new Editor(nullptr));
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    ASSERT_TRUE(pumpUntil(
        [&] { return evaluate(webView, @"String(window.orizonDebug !== undefined)") == "true"; },
        kPageTimeout));
    evaluate(webView, @"document.querySelector('.field').dispatchEvent("
                       "new PointerEvent('pointerover', {bubbles: true})); ''");
    // Long enough for a reported cursor to reach the view, as it does within the cursor test's
    // waits.
    pumpUntil([] { return false; }, std::chrono::milliseconds(500));
    EXPECT_TRUE(cursorRectsOf(webView).empty());
    [[NSCursor crosshairCursor] set];
    [webView cursorUpdate:[NSEvent enterExitEventWithType:NSEventTypeCursorUpdate
                                                 location:NSMakePoint(10, 10)
                                            modifierFlags:0
                                                timestamp:0
                                             windowNumber:window.windowNumber
                                                  context:nil
                                              eventNumber:0
                                           trackingNumber:0
                                                 userData:nullptr]];
    EXPECT_NE([NSCursor currentCursor], [NSCursor resizeUpDownCursor]);
    EXPECT_EQ(editor->removed(), kResultOk);
    [window close];
}

// A view that shows the page's cursors itself (the Audio Unit's, which Logic hosts out of process,
// where WebKit's cursors and one set on a mouse move do not show) gives AppKit the page's cursor
// through its cursor rectangle and cursorUpdate:. A pointer over each kind of control the page
// has, and over nothing in particular, gives that control's cursor.
TEST_F(EditorTest, ShowsThePagesCursorForWhatIsUnderThePointer)
{
    NSView* parent = [[NSView alloc] init];
    OrizonTestCursorWindow* window = [[OrizonTestCursorWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, Editor::kDefaultWidth, Editor::kDefaultHeight)
                  styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered
                      defer:NO];
    window.releasedWhenClosed = NO;
    window.contentView = parent;
    EditorCore core(nullptr);
    core.setCursors(WebView::Cursors::kShownByView);
    core.attach((__bridge void*)parent);
    WebView& view = *core.webView();
    auto* webView = (__bridge WKWebView*)view.nativeView();
    [window watch:webView];
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
    auto* diagonal = (__bridge NSCursor*)drawnDiagonalResizeCursorForTests();
    if (@available(macOS 15.0, *)) {
        diagonal = [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionBottomRight
                                              inDirections:NSCursorFrameResizeDirectionsAll];
    }
    cases.emplace_back(@"#resize-grip", diagonal);
    NSEvent* update = [NSEvent enterExitEventWithType:NSEventTypeCursorUpdate
                                             location:NSMakePoint(10, 10)
                                        modifierFlags:0
                                            timestamp:0
                                         windowNumber:window.windowNumber
                                              context:nil
                                          eventNumber:0
                                       trackingNumber:0
                                             userData:nullptr];
    for (const auto& [selector, expected] : cases) {
        SCOPED_TRACE(selector.UTF8String);
        const int invalidations = [window invalidationCount];
        evaluate(webView, [NSString stringWithFormat:@"document.querySelector('%@').dispatchEvent("
                                                      "new PointerEvent('pointerover', "
                                                      "{bubbles: true})); ''",
                                                     selector]);
        EXPECT_TRUE(pumpUntil([&] { return (__bridge NSCursor*)cursorForTests(view) == expected; },
                              std::chrono::seconds(2)));
        EXPECT_GT([window invalidationCount], invalidations)
            << "AppKit is told to ask for the view's new cursor rectangle";
        const std::vector<CursorRect> rects = cursorRectsOf(webView);
        ASSERT_EQ(rects.size(), 1U);
        EXPECT_TRUE(NSEqualRects(rects[0].rect, webView.bounds));
        EXPECT_EQ(rects[0].cursor, expected);
        [[NSCursor crosshairCursor] set]; // None the page uses: cursorUpdate: must set the cursor.
        [webView cursorUpdate:update];
        EXPECT_EQ([NSCursor currentCursor], expected);
    }

    // A cursor without a macOS match is left to WebKit: the view gives AppKit none of its own, and
    // AppKit is told to drop the previous control's rectangle.
    const int invalidations = [window invalidationCount];
    evaluate(webView, @"const grab = document.querySelector('#labels'); grab.style.cursor = 'grab';"
                       "grab.dispatchEvent(new PointerEvent('pointerover', {bubbles: true})); ''");
    EXPECT_TRUE(
        pumpUntil([&] { return cursorForTests(view) == nullptr; }, std::chrono::seconds(2)));
    EXPECT_GT([window invalidationCount], invalidations);
    EXPECT_TRUE(cursorRectsOf(webView).empty());
    core.detach();
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
// from the system symbol, outlined, with its hot spot at the arrows' centre. Drawn on any macOS, so
// the tests check it whatever macOS they run on.
TEST_F(EditorTest, DrawsADiagonalResizeCursorForMacOSBefore15)
{
    auto* cursor = (__bridge NSCursor*)drawnDiagonalResizeCursorForTests();
    ASSERT_NE(cursor, nil);
    const NSSize size = cursor.image.size;
    EXPECT_GT(size.width, 0.0);
    EXPECT_GT(size.height, 0.0);
    EXPECT_DOUBLE_EQ(cursor.hotSpot.x, size.width / 2);
    EXPECT_DOUBLE_EQ(cursor.hotSpot.y, size.height / 2);

    // Black on a white outline, so it shows over the page's dark panel as over light: the image
    // holds opaque pixels of both.
    NSBitmapImageRep* pixels = [[NSBitmapImageRep alloc]
        initWithCGImage:[cursor.image CGImageForProposedRect:nil context:nil hints:nil]];
    bool dark = false;
    bool light = false;
    for (NSInteger y = 0; y < pixels.pixelsHigh; ++y) {
        for (NSInteger x = 0; x < pixels.pixelsWide; ++x) {
            NSColor* colour =
                [[pixels colorAtX:x y:y] colorUsingColorSpace:[NSColorSpace genericRGBColorSpace]];
            if (colour.alphaComponent > 0.9) {
                dark = dark || colour.brightnessComponent < 0.2;
                light = light || colour.brightnessComponent > 0.8;
            }
        }
    }
    EXPECT_TRUE(dark) << "a black glyph";
    EXPECT_TRUE(light) << "a white outline";
}

// Frames flow while the page asks for them, and host state changes reach the page.
// Test windows are occluded (an unbundled test process never becomes a foreground app), so
// WebKit pauses requestAnimationFrame here; the test asks for frames itself, through the page's
// real frame requests.
TEST_F(EditorTest, StreamsFramesAndReportsHostStateChanges)
{
    auto engine = std::make_shared<engine::Engine>();
    engine->prepare(48000.0);
    engine->settings().set(protocol::SettingId::kTimeBase,
                           static_cast<double>(protocol::TimeBase::kFree));
    engine->settings().set(protocol::SettingId::kFreeSpan, 1.0); // Free: a frame per request.
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
    restored.set(protocol::SettingId::kTimeBase, static_cast<double>(protocol::TimeBase::kFree));
    restored.set(protocol::SettingId::kFreeSpan, 1.0); // As before: only the level changes.
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

// With the embedded fonts as WebKit renders them, everything fits at the minimum size and, with the
// wordmark, at the default size, in every state that changes the labels' widths.
TEST_F(EditorTest, FitsTheMinimumAndDefaultSizesInEveryState)
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
    // The minimum size with the mark alone, then the default size with the wordmark.
    for (ViewRect size : {minimum, ViewRect(0, 0, Editor::kDefaultWidth, Editor::kDefaultHeight)}) {
        ASSERT_EQ(editor->onSize(&size), kResultTrue);
        ASSERT_TRUE(pumpUntil(
            [&] {
                return evaluate(webView, @"String(innerWidth)") == std::to_string(size.getWidth());
            },
            std::chrono::seconds(5)));
        const bool isDefault = size.getWidth() == Editor::kDefaultWidth;
        // The page shows or hides it on the resize event that follows.
        EXPECT_TRUE(pumpUntil(
            [&] {
                return evaluate(webView, @(fit::kWordmarkScript)) ==
                       (isDefault ? "shown" : "hidden");
            },
            std::chrono::seconds(5)))
            << "the wordmark at " << size.getWidth();
        for (const fit::State& state : fit::states()) {
            engine::Settings settings;
            for (const auto& [id, value] : state.settings) {
                ASSERT_TRUE(settings.set(id, value)) << state.name; // Not refused.
            }
            engine->settings().replace(settings);
            // A state sync or more.
            pumpUntil([] { return false; }, std::chrono::milliseconds(400));
            if (!state.script.empty()) {
                evaluate(webView,
                         [NSString stringWithUTF8String:std::string(state.script).c_str()]);
                pumpUntil([] { return false; }, std::chrono::milliseconds(200));
            }
            EXPECT_EQ(evaluate(webView, @(fit::kOverflowScript)), "")
                << state.name << " at " << size.getWidth();
            EXPECT_LE(std::stoi(evaluate(webView, @(fit::kChromeScript))), Editor::kChromeHeight)
                << state.name << " at " << size.getWidth();
        }
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
    evaluate(
        webView,
        [NSString stringWithFormat:@"window.webkit.messageHandlers.orizon.postMessage("
                                   @"JSON.stringify({type: '%s', %s})); ''",
                                   std::string(messageName(protocol::MessageType::kResize)).c_str(),
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

// The About popover shows the version the host lists for the plugin, also when the editor cannot
// reach the engine, when a user is most likely to report it.
TEST_F(EditorTest, ShowsTheVersionTheHostIsGiven)
{
    for (const bool withEngine : {true, false}) {
        NSView* parent = [[NSView alloc] init];
        NSWindow* window = makeWindow(parent);
        std::vector<std::string> received;
        auto editor = withEngine ? makeEditor(received) : makeEditor(received, nullptr);
        ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
        auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
        EXPECT_TRUE(pumpUntil(
            [&] {
                return evaluate(webView, @"document.getElementById('about-version')?.textContent "
                                         @"?? ''") == engine::kVersionString;
            },
            kPageTimeout))
            << (withEngine ? "with" : "without") << " an engine";
        EXPECT_EQ(editor->removed(), kResultOk);
        [window close];
    }
}

// About us opens the vendor's contact page in the default browser, never in the editor. A build
// without the brand has no contact page: it offers none, and a request for one opens nothing.
TEST_F(EditorTest, AboutUsOpensTheVendorsPageOnlyInABrandedBuild)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    std::vector<std::string> opened;
    editor->setLinkOpener([&opened](std::string_view url) { opened.emplace_back(url); });
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));
    const bool branded = SINELESS_BRANDED == 1;
    const std::string shown =
        evaluate(webView, @"String(!(document.getElementById('contact')?.hidden ?? true))");
    EXPECT_EQ(shown, branded ? "true" : "false") << "About us offered";
    if (branded) {
        evaluate(webView, @"document.getElementById('about-details').click();"
                          @"document.getElementById('contact').click(); ''");
    } else {
        evaluate(webView, @"window.webkit.messageHandlers.orizon.postMessage("
                          @"JSON.stringify({type: 'contact'})); ''");
    }
    // Every message the page sent before this one has been handled, and the work the editor
    // posted for them has run: the dispatcher's queue is first in, first out.
    evaluate(webView, @"window.webkit.messageHandlers.orizon.postMessage("
                      @"JSON.stringify({type: 'arm'})); ''");
    ASSERT_TRUE(pumpUntil([&] { return saw(received, "arm"); }, kPageTimeout));
    bool drained = false;
    auto dispatcher = UiDispatcher::create();
    dispatcher->post([&drained] { drained = true; });
    ASSERT_TRUE(pumpUntil([&] { return drained; }, std::chrono::seconds(5)));
    if (branded) {
        EXPECT_EQ(opened, std::vector<std::string>{branding::kContactUrl});
    } else {
        EXPECT_TRUE(opened.empty()) << opened.front();
    }
    EXPECT_STREQ(webView.URL.absoluteString.UTF8String, "orizon://ui/index.html");
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

// Behind the page, while it loads or reloads, the editor shows the page's own background colour,
// so nothing flashes another colour.
TEST_F(EditorTest, ShowsThePageBackgroundBehindThePage)
{
    NSView* parent = [[NSView alloc] init];
    NSWindow* window = makeWindow(parent);
    std::vector<std::string> received;
    auto editor = makeEditor(received);
    ASSERT_EQ(editor->attached((__bridge void*)parent, kPlatformTypeNSView), kResultTrue);
    auto* webView = (__bridge WKWebView*)editor->webView()->nativeView();
    ASSERT_TRUE(pumpUntil([&] { return hasSnapshot(webView); }, kPageTimeout));
    const CGColorRef native = webView.superview.layer.backgroundColor;
    ASSERT_NE(native, nullptr);
    NSColor* colour =
        [[NSColor colorWithCGColor:native] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    const std::string expected = "rgb(" + std::to_string(std::lround(colour.redComponent * 255)) +
                                 ", " + std::to_string(std::lround(colour.greenComponent * 255)) +
                                 ", " + std::to_string(std::lround(colour.blueComponent * 255)) +
                                 ")";
    EXPECT_EQ(evaluate(webView, @"getComputedStyle(document.body).backgroundColor"), expected);
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

// The pointer as a view's hold sees it, recording what the hold does to it; the real one is never
// touched, as the tests run in the developer's session.
class FakePointer final : public PointerControl
{
public:
    ScreenPoint position() override { return at; }
    bool primaryButtonDown() override { return buttonDown; }
    double now() override { return 0; } // Before every event: none is from before a hold.
    void hide() override { record("hide"); }
    void show() override { record("show"); }
    void freeze() override { record("freeze"); }
    void unfreeze() override { record("unfreeze"); }
    void warp(ScreenPoint point) override
    {
        warpedTo = point;
        record("warp");
    }
    // The window server's cursor-in-background setting, as the view turns it on or off.
    bool setCursorInBackground(bool on)
    {
        withSetting.emplace_back(on ? "background on" : "background off");
        return settingWorks;
    }

    ScreenPoint at{};
    bool buttonDown = true;
    bool settingWorks = true;
    // What the hold did to the pointer, and the same with the setting's changes among it.
    std::vector<std::string> calls;
    std::vector<std::string> withSetting;
    ScreenPoint warpedTo{};

private:
    void record(const char* call)
    {
        calls.emplace_back(call);
        withSetting.emplace_back(call);
    }
};

// A kind of view the hold runs in: who shows its cursors, and how it hides the pointer.
struct HoldView
{
    WebView::Cursors cursors;
    WebView::PointerHiding hiding;
};

std::string nameOf(const HoldView& view)
{
    return std::string(view.cursors == WebView::Cursors::kShownByView ? "ShownByView"
                                                                      : "ShownByWebKit") +
           (view.hiding == WebView::PointerHiding::kWithBackgroundCursor ? "WithBackgroundCursor"
                                                                         : "Public");
}

std::ostream& operator<<(std::ostream& out, const HoldView& view)
{
    return out << nameOf(view);
}

// The pointer hold in every kind of view: the VST3 editor's, whose cursors WebKit shows, and the
// Audio Unit's, which shows them itself, each hiding the pointer with the public calls alone or
// with the cursor-in-background setting, as the Audio Unit does in Apple's hosts.
class PointerHoldTest : public EditorTest, public ::testing::WithParamInterface<HoldView>
{
protected:
    void SetUp() override
    {
        open(nullptr);
        // What the page's drag field does when a value is pressed.
        evaluate(webView, @"window.travels = []; window.orizonPointerHold.onTravel = "
                           "(travel, fine) => window.travels.push(`${travel} ${fine}`); ''");
    }

    void TearDown() override
    {
        core.detach();
        [window close];
    }

    // Opens the view with the fake pointer, in a process whose main bundle has `bundleType`
    // (nullptr: this one's), and waits for the page.
    void open(const char* bundleType)
    {
        parent = [[NSView alloc] init];
        window = makeWindow(parent);
        setPointerControlForTests(&pointer);
        setCursorInBackgroundForTests(
            [this](bool on) { return pointer.setCursorInBackground(on); });
        setBundleTypeForTests(bundleType);
        core.setCursors(GetParam().cursors);
        core.setPointerHiding(GetParam().hiding);
        core.attach((__bridge void*)parent);
        setPointerControlForTests(nullptr);
        setCursorInBackgroundForTests({});
        setBundleTypeForTests(nullptr);
        webView = (__bridge WKWebView*)core.webView()->nativeView();
        ASSERT_TRUE(pumpUntil(
            [&] {
                return evaluate(webView, @"String(window.orizonDebug !== undefined)") == "true";
            },
            kPageTimeout));
    }

    // A left-button press on the view, where AppKit would deliver it; its location on the screen.
    ScreenPoint pressOnView()
    {
        NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                           location:NSMakePoint(40, 40)
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:window.windowNumber
                                            context:nil
                                        eventNumber:0
                                         clickCount:1
                                           pressure:1];
        const CGPoint point = CGEventGetLocation(down.CGEvent);
        [webView mouseDown:down];
        return {.x = point.x, .y = point.y};
    }

    void startHold() { evaluate(webView, @"window.orizonPointerHold.start(); ''"); }

    std::string travels() { return evaluate(webView, @"window.travels.join(', ')"); }

    // Sends a mouse event through the application, where its local monitors see it.
    void sendThroughApplication(CGEventType type, int64_t dy, CGEventFlags flags = 0)
    {
        CGEventRef event =
            CGEventCreateMouseEvent(nullptr, type, CGPointMake(10, 10), kCGMouseButtonLeft);
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, dy);
        CGEventSetFlags(event, flags);
        CGEventSetTimestamp(event, mach_absolute_time()); // Stamped now, as the system stamps it.
        [NSApp postEvent:[NSEvent eventWithCGEvent:event] atStart:NO];
        CFRelease(event);
        while (NSEvent* next =
                   [NSApp nextEventMatchingMask:NSEventMaskAny
                                      untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]
                                         inMode:NSDefaultRunLoopMode
                                        dequeue:YES]) {
            [NSApp sendEvent:next];
        }
    }

    FakePointer pointer;
    NSView* parent = nil;
    NSWindow* window = nil;
    EditorCore core{nullptr};
    WKWebView* webView = nil;
};

TEST_P(PointerHoldTest, HidesAndFreezesThePointerWhenThePageAsks)
{
    const ScreenPoint press = pressOnView();
    pointer.at = {.x = press.x, .y = press.y - 9}; // Moved before the page's start arrived.
    startHold();
    ASSERT_TRUE(pumpUntil([&] { return travels() == "-9 false"; }, std::chrono::seconds(2)))
        << travels();
    EXPECT_EQ(pointer.calls, (std::vector<std::string>{"hide", "freeze"}));
    evaluate(webView, @"window.orizonPointerHold.end(); ''");
    ASSERT_TRUE(pumpUntil([&] { return pointer.calls.size() == 5; }, std::chrono::seconds(2)));
    EXPECT_EQ(pointer.calls.back(), "show");
    EXPECT_DOUBLE_EQ(pointer.warpedTo.x, press.x); // Back where it was pressed.
    EXPECT_DOUBLE_EQ(pointer.warpedTo.y, press.y);
}

TEST_P(PointerHoldTest, FollowsTheMouseThroughTheApplicationsEventsUntilItsRelease)
{
    pointer.at = pressOnView(); // Frozen where it was pressed, as on a press.
    startHold();
    ASSERT_TRUE(pumpUntil([&] { return travels() == "0 false"; }, std::chrono::seconds(2)));
    sendThroughApplication(kCGEventLeftMouseDragged, -20);
    sendThroughApplication(kCGEventLeftMouseDragged, -20, kCGEventFlagMaskShift);
    sendThroughApplication(kCGEventLeftMouseUp, 0, kCGEventFlagMaskShift);
    ASSERT_TRUE(pumpUntil([&] { return pointer.calls.size() == 4; }, std::chrono::seconds(2)))
        << travels();
    // The last report arrives with the release, however the reports were coalesced.
    EXPECT_TRUE(pumpUntil([&] { return travels().ends_with("-40 true"); }, std::chrono::seconds(2)))
        << "travels: " << travels();
    // Still at the press: shown there with no warp.
    EXPECT_EQ(pointer.calls, (std::vector<std::string>{"hide", "freeze", "unfreeze", "show"}));
    // The monitor went with the hold: the next drag's motion counts once, not once per drag so far.
    evaluate(webView, @"window.travels = []; ''");
    pointer.at = pressOnView();
    startHold();
    ASSERT_TRUE(pumpUntil([&] { return travels() == "0 false"; }, std::chrono::seconds(2)));
    sendThroughApplication(kCGEventLeftMouseDragged, -20);
    sendThroughApplication(kCGEventLeftMouseUp, 0);
    ASSERT_TRUE(pumpUntil([&] { return pointer.calls.size() == 8; }, std::chrono::seconds(2)));
    EXPECT_TRUE(
        pumpUntil([&] { return travels().ends_with("-20 false"); }, std::chrono::seconds(2)))
        << "travels: " << travels();
}

TEST_P(PointerHoldTest, EndsWhenTheButtonIsFoundUpWithNoRelease)
{
    pressOnView();
    startHold();
    ASSERT_TRUE(pumpUntil([&] { return pointer.calls.size() == 2; }, std::chrono::seconds(2)));
    pollPointerHoldForTests(*core.webView());
    EXPECT_EQ(pointer.calls.size(), 2U); // Still held down.
    pointer.buttonDown = false;
    // The view's own check finds it, as the test's call would.
    EXPECT_TRUE(pumpUntil([&] { return pointer.calls.size() == 5; }, std::chrono::seconds(2)));
    EXPECT_EQ(pointer.calls.back(), "show");
    // The view let go of it too: the next press holds again.
    pointer.buttonDown = true;
    pressOnView();
    startHold();
    ASSERT_TRUE(pumpUntil([&] { return pointer.calls.size() == 7; }, std::chrono::seconds(2)));
    EXPECT_EQ(pointer.calls[5], "hide");
    EXPECT_EQ(pointer.calls[6], "freeze");
}

TEST_P(PointerHoldTest, ShowsThePointerWhenClosedDuringAHold)
{
    pressOnView();
    startHold();
    ASSERT_TRUE(pumpUntil([&] { return pointer.calls.size() == 2; }, std::chrono::seconds(2)));
    core.detach();
    EXPECT_EQ(pointer.calls,
              (std::vector<std::string>{"hide", "freeze", "warp", "unfreeze", "show"}));
}

// A page process that dies mid-drag cannot end its hold: the view does, with the button still down.
TEST_P(PointerHoldTest, ShowsThePointerWhenThePageProcessEndsDuringAHold)
{
    pressOnView();
    startHold();
    ASSERT_TRUE(pumpUntil([&] { return pointer.calls.size() == 2; }, std::chrono::seconds(2)));
    ASSERT_TRUE(terminateContentProcessForTests(*core.webView()));
    EXPECT_TRUE(pumpUntil([&] { return pointer.calls.size() == 5; }, std::chrono::seconds(5)));
    EXPECT_EQ(pointer.calls,
              (std::vector<std::string>{"hide", "freeze", "warp", "unfreeze", "show"}));
}

TEST_P(PointerHoldTest, HoldsNothingWithoutAPressOnTheView)
{
    startHold();
    pumpUntil([] { return false; }, std::chrono::milliseconds(300));
    EXPECT_TRUE(pointer.calls.empty());
    EXPECT_EQ(travels(), "");
}

// The kinds of view, named.
const auto kHoldViewName = [](const ::testing::TestParamInfo<HoldView>& info) {
    return nameOf(info.param);
};

// Each kind of view with each way of hiding the pointer, or with one.
std::vector<HoldView> holdViews(std::initializer_list<WebView::PointerHiding> hidings)
{
    std::vector<HoldView> views;
    for (const auto cursors : {WebView::Cursors::kShownByWebKit, WebView::Cursors::kShownByView}) {
        for (const auto hiding : hidings) {
            views.push_back({.cursors = cursors, .hiding = hiding});
        }
    }
    return views;
}

INSTANTIATE_TEST_SUITE_P(
    Views, PointerHoldTest,
    ::testing::ValuesIn(holdViews({WebView::PointerHiding::kPublic,
                                   WebView::PointerHiding::kWithBackgroundCursor})),
    kHoldViewName);

// The VST3 editor's view and other hosts' Audio Unit views: the setting is never touched.
class PublicPointerHidingTest : public PointerHoldTest
{
};

TEST_P(PublicPointerHidingTest, LeavesTheBackgroundCursorAloneByDefault)
{
    pointer.at = pressOnView();
    startHold();
    ASSERT_TRUE(pumpUntil([&] { return pointer.calls.size() == 2; }, std::chrono::seconds(2)));
    sendThroughApplication(kCGEventLeftMouseUp, 0);
    ASSERT_TRUE(pumpUntil([&] { return pointer.calls.size() == 4; }, std::chrono::seconds(2)));
    EXPECT_EQ(pointer.withSetting, pointer.calls);
}

INSTANTIATE_TEST_SUITE_P(Views, PublicPointerHidingTest,
                         ::testing::ValuesIn(holdViews({WebView::PointerHiding::kPublic})),
                         kHoldViewName);

// The Audio Unit's view in Logic, where the public calls report success but leave the pointer
// visible unless the process's window-server connection has the cursor-in-background setting on:
// it is on exactly while a hold hides the pointer.
class BackgroundCursorHoldTest : public PointerHoldTest
{
protected:
    // A press, the page's start and a release after the pointer moved; the hold's calls since.
    std::vector<std::string> holdAndRelease()
    {
        const std::size_t before = pointer.withSetting.size();
        const std::size_t held = pointer.calls.size() + 2; // Hidden and frozen.
        const ScreenPoint press = pressOnView();
        pointer.at = {.x = press.x, .y = press.y - 9};
        startHold();
        EXPECT_TRUE(
            pumpUntil([&] { return pointer.calls.size() == held; }, std::chrono::seconds(2)));
        sendThroughApplication(kCGEventLeftMouseUp, 0);
        EXPECT_TRUE(pumpUntil([&] { return pointer.withSetting.size() == before + 7; },
                              std::chrono::seconds(2)));
        return {pointer.withSetting.begin() + static_cast<std::ptrdiff_t>(before),
                pointer.withSetting.end()};
    }

    // A press and the page's start: the hold is on.
    void hold()
    {
        pressOnView();
        startHold();
        ASSERT_TRUE(pumpUntil([&] { return pointer.calls.size() == 2; }, std::chrono::seconds(2)));
        ASSERT_EQ(pointer.withSetting,
                  (std::vector<std::string>{"background on", "hide", "freeze"}));
    }
};

// A hold from the press to its end, with the setting on around it.
std::vector<std::string> holdWithBackgroundCursor()
{
    return {"background on", "hide", "freeze", "warp", "unfreeze", "show", "background off"};
}

TEST_P(BackgroundCursorHoldTest, HidesThePointerWithTheBackgroundCursorForAnAppleHost)
{
    EXPECT_EQ(holdAndRelease(), holdWithBackgroundCursor());
    EXPECT_EQ(holdAndRelease(), holdWithBackgroundCursor()); // Each hold turns it on and off again.
}

TEST_P(BackgroundCursorHoldTest, HidesWithThePublicCallsWhenTheSettingIsUnavailable)
{
    pointer.settingWorks = false;
    EXPECT_EQ(holdAndRelease(), holdWithBackgroundCursor());
    EXPECT_EQ(pointer.calls,
              (std::vector<std::string>{"hide", "freeze", "warp", "unfreeze", "show"}));
}

TEST_P(BackgroundCursorHoldTest, TurnsTheBackgroundCursorOffWhenTheButtonIsFoundUp)
{
    hold();
    pointer.buttonDown = false;
    pollPointerHoldForTests(*core.webView());
    EXPECT_TRUE(
        pumpUntil([&] { return pointer.withSetting.size() == 7; }, std::chrono::seconds(2)));
    EXPECT_EQ(pointer.withSetting, holdWithBackgroundCursor());
}

TEST_P(BackgroundCursorHoldTest, TurnsTheBackgroundCursorOffWhenClosedDuringAHold)
{
    hold();
    core.detach();
    EXPECT_EQ(pointer.withSetting, holdWithBackgroundCursor());
}

TEST_P(BackgroundCursorHoldTest, TurnsTheBackgroundCursorOffWhenThePageProcessEndsDuringAHold)
{
    hold();
    ASSERT_TRUE(terminateContentProcessForTests(*core.webView()));
    EXPECT_TRUE(
        pumpUntil([&] { return pointer.withSetting.size() == 7; }, std::chrono::seconds(5)));
    EXPECT_EQ(pointer.withSetting, holdWithBackgroundCursor());
}

INSTANTIATE_TEST_SUITE_P(
    Views, BackgroundCursorHoldTest,
    ::testing::ValuesIn(holdViews({WebView::PointerHiding::kWithBackgroundCursor})), kHoldViewName);

// A host that loads an Audio Unit in an XPC service runs it where the pointer cannot be frozen:
// there the page gets no hold, and a drag follows the pointer as before.
class OutOfProcessPointerHoldTest : public PointerHoldTest
{
protected:
    void SetUp() override { open("XPC!"); }
};

TEST_P(OutOfProcessPointerHoldTest, LeavesThePointerAloneInAnXpcService)
{
    EXPECT_EQ(evaluate(webView, @"typeof window.orizonPointerHold"), "undefined");
    pressOnView();
    evaluate(webView, @"window.webkit.messageHandlers.orizonPointerHold?.postMessage('start'); ''");
    pumpUntil([] { return false; }, std::chrono::milliseconds(300));
    EXPECT_TRUE(pointer.calls.empty());
}

INSTANTIATE_TEST_SUITE_P(
    Views, OutOfProcessPointerHoldTest,
    ::testing::ValuesIn(holdViews({WebView::PointerHiding::kPublic,
                                   WebView::PointerHiding::kWithBackgroundCursor})),
    kHoldViewName);

// XPC services, where a host may load Audio Units, cannot freeze the pointer: there the page gets
// no hold and drags follow the pointer. Plugin hosts are applications.
TEST(PointerHoldGate, HoldsThePointerOutsideXpcServicesOnly)
{
    EXPECT_FALSE(holdsPointerInBundleOfTypeForTests("XPC!"));
    EXPECT_TRUE(holdsPointerInBundleOfTypeForTests("APPL"));
    EXPECT_TRUE(holdsPointerInBundleOfTypeForTests(nullptr)); // A bare executable, as the tests.
}

// The setting a hold turns on in Apple's hosts, as the window server keeps it for this process's
// connection: on after it is turned on, off after it is turned off.
TEST(CursorInBackground, SetsTheWindowServersCursorInBackgroundProperty)
{
    [NSApplication sharedApplication]; // Connects this process to the window server.
    using Connection = int (*)();
    using CopyProperty = CGError (*)(int, int, CFStringRef, CFTypeRef*);
    const auto connection =
        reinterpret_cast<Connection>(dlsym(RTLD_DEFAULT, "_CGSDefaultConnection"));
    const auto copyProperty =
        reinterpret_cast<CopyProperty>(dlsym(RTLD_DEFAULT, "CGSCopyConnectionProperty"));
    ASSERT_NE(connection, nullptr);
    ASSERT_NE(copyProperty, nullptr);
    const auto read = [&] {
        CFTypeRef value = nullptr;
        EXPECT_EQ(copyProperty(connection(), connection(), CFSTR("SetsCursorInBackground"), &value),
                  kCGErrorSuccess);
        const bool on = value != nullptr && CFEqual(value, kCFBooleanTrue);
        if (value != nullptr) {
            CFRelease(value);
        }
        return on;
    };
    ASSERT_TRUE(setRealCursorInBackgroundForTests(true));
    EXPECT_TRUE(read());
    ASSERT_TRUE(setRealCursorInBackgroundForTests(false));
    EXPECT_FALSE(read());
}

} // namespace
} // namespace orizon::editor
