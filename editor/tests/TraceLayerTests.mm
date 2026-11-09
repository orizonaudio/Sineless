#include "MacPageTesting.h"
#include "engine/MonotonicClock.h"
#include "mac/TraceLayerMac.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace orizon::editor {
namespace {

using namespace std::chrono_literals;
using testing::pumpUntil;

// A frame as the engine sends it, and the trace view the page would send with it: the first of
// the renderer's parity scenes (protocol/tests/data/trace-pictures.json).
struct Scene
{
    std::vector<std::byte> frame;
    TraceView view;
};

Scene firstScene()
{
    std::ifstream file(std::string(ORIZON_PROTOCOL_TEST_DATA_DIR) + "/trace-pictures.json");
    const nlohmann::json entry = nlohmann::json::parse(file).at("scenes").at(0);
    Scene scene;
    NSData* bytes =
        [[NSData alloc] initWithBase64EncodedString:@(entry.at("frame").get<std::string>().c_str())
                                            options:0];
    const auto* data = static_cast<const std::byte*>(bytes.bytes);
    scene.frame.assign(data, data + bytes.length);
    scene.view = *TraceView::parse(entry.at("traceView"));
    return scene;
}

// What the editor gives the layer: frames on request, counted.
struct Source
{
    std::mutex mutex;
    std::vector<std::byte> frame;
    // Each take hands over a new frame (as with a moving trace), or only after a request. Set by
    // a test while the display thread takes frames.
    std::atomic<bool> always{false};
    // Frames to hand over in all (none: no end): frozen, or Single after its capture, there is one.
    std::optional<int> limit;
    // The diagnostic switch, as the editor reads it.
    bool frameBarcode = false;
    bool ready = false;
    std::atomic<int> requests{0};
    std::atomic<int> takes{0};
    // Whether every request came on the render thread.
    bool requestsOnRenderThread = true;
    std::atomic<int> losses{0};
    // Where a frame's time from request to ready goes, as the editor's frame flow reports it: this
    // source's frames are ready at once, so the time is how late the request's timer fired.
    engine::FramePacing* pacing = nullptr;

    TraceLayer::Hooks hooks()
    {
        return {.requestFrame =
                    [this](std::uint64_t lateNanos) {
                        if (pacing != nullptr) {
                            pacing->frameMade(lateNanos);
                        }
                        const std::scoped_lock lock(mutex);
                        ++requests;
                        ready = true;
                        requestsOnRenderThread =
                            requestsOnRenderThread &&
                            [NSThread.currentThread.name isEqualToString:@"Sineless traces"];
                    },
                .takeFrame =
                    [this](std::vector<std::byte>& into) {
                        const std::scoped_lock lock(mutex);
                        if ((!always && !ready) || (limit && takes.load() >= *limit)) {
                            return false;
                        }
                        ready = false;
                        ++takes;
                        into = frame;
                        return true;
                    },
                .lost = [this] { ++losses; },
                .frameBarcode = frameBarcode};
    }
};

class TraceLayerTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { [NSApplication sharedApplication]; }

    void SetUp() override
    {
        scene = firstScene();
        source.frame = scene.frame;
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, 1080, 580)
                                             styleMask:NSWindowStyleMaskTitled
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        window.releasedWhenClosed = NO;
        container = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1080, 580)];
        above = [[NSView alloc] initWithFrame:container.bounds];
        [container addSubview:above];
        window.contentView = container;
        [window orderFrontRegardless];
        // On screen, whatever the display reports: an idle one reports every window occluded,
        // though the display link still runs.
        TraceLayer::setOcclusionForTesting(false);
        pacing = std::make_unique<engine::FramePacing>(
            static_cast<double>(window.screen.maximumFramesPerSecond));
        source.pacing = pacing.get();
        layer = TraceLayer::create(container, above, *pacing, source.hooks());
    }

    void TearDown() override
    {
        layer.reset();
        TraceLayer::setOcclusionForTesting(std::nullopt);
        [window orderOut:nil];
        [window close];
    }

    // The layer shown and drawing: the page up and its trace view sent.
    void show()
    {
        layer->setPageShown(true);
        layer->setTraceView(scene.view);
    }

    std::uint64_t submitted() const { return layer->statsForTesting().submitted; }

    // Whether `met` held, and if not, whether the display link called back on at least
    // kRanFraction of the refreshes since `started` (`callbacks` then): only its callbacks draw,
    // take frames and find failed buffers. On a stalled CI display the test then skips
    // (MacPageTesting.h, "Waits on the display").
    testing::DisplayWait judge(bool met, std::uint64_t callbacks,
                               std::chrono::steady_clock::time_point started,
                               const std::string& what) const
    {
        if (met) {
            return {.met = true, .displayRan = true, .message = {}};
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        const auto stats = layer->statsForTesting();
        const std::uint64_t given = stats.callbacks - callbacks;
        const double hertz = static_cast<double>(window.screen.maximumFramesPerSecond);
        const double expected = testing::expectedRefreshes(elapsed, hertz);
        const bool ran = testing::linkRan(given, expected, stats.linkRunning);
        std::ostringstream message;
        message << what << ": not in " << elapsed.count()
                << " ms, in which the display link called "
                << "back " << given << " times of " << std::lround(expected) << " refreshes ("
                << std::lround(static_cast<double>(given) * 1000.0 /
                               static_cast<double>(std::max<std::int64_t>(elapsed.count(), 1)))
                << " a second; the screen reports " << hertz << " Hz); link "
                << (stats.linkRunning ? "running" : "paused by the layer") << ", layer hidden "
                << (layer->view().hidden ? "yes" : "no") << (ran ? "" : ": the display stalled");
        return {.met = false, .displayRan = ran, .message = message.str()};
    }

    // Waits for `condition`, which only the display link's callbacks bring about.
    testing::DisplayWait drawsWithin(const std::function<bool()>& condition,
                                     std::chrono::milliseconds within, const std::string& what)
    {
        const std::uint64_t callbacks = layer->statsForTesting().callbacks;
        const auto started = std::chrono::steady_clock::now();
        return judge(pumpUntil(condition, within), callbacks, started, what);
    }

    // A picture submitted after `before`, counted before the change that should draw one: the
    // link may submit it as soon as the change is made, before the test starts waiting.
    testing::DisplayWait presentsSince(std::uint64_t before,
                                       const std::string& what = "a picture presented",
                                       std::chrono::milliseconds within = 2s)
    {
        return drawsWithin([&] { return submitted() > before; }, within, what);
    }

    testing::DisplayWait presents(std::chrono::milliseconds within = 2s)
    {
        return presentsSince(submitted(), "a picture presented", within);
    }

    // The screen's rectangle in the container's coordinates (from the bottom), and in device
    // pixels.
    NSRect expectedFrame() const
    {
        const NSSize size = container.bounds.size;
        const ScreenRect rect = ScreenRect::of(size.width, size.height);
        return NSMakeRect(rect.x, size.height - rect.y - rect.height, rect.width, rect.height);
    }

    Scene scene;
    Source source;
    NSWindow* window = nil;
    NSView* container = nil;
    NSView* above = nil;
    std::unique_ptr<engine::FramePacing> pacing;
    std::unique_ptr<TraceLayer> layer;
};

// Below the page, which takes every event, and inside the border's curve.
TEST_F(TraceLayerTest, SitsBelowThePageInsideTheScreensCorners)
{
    NSView* view = layer->view();
    ASSERT_EQ(container.subviews.count, 2U);
    EXPECT_EQ(container.subviews[0], view);
    EXPECT_EQ(container.subviews[1], above);
    EXPECT_EQ([view hitTest:NSMakePoint(NSMidX(view.frame), NSMidY(view.frame))], nil);
    EXPECT_EQ(view.layer.cornerRadius, protocol::kScreenRadius - protocol::kScreenBorder);
    EXPECT_TRUE(view.layer.masksToBounds);
}

// Nothing shows before the page says how to draw (its colours, its view): the page's colour
// behind it until then.
TEST_F(TraceLayerTest, DrawsNothingUntilThePageHasSentItsTraceView)
{
    layer->setPageShown(true);
    EXPECT_FALSE(presents(300ms).met);
    EXPECT_TRUE(layer->view().hidden);
    const auto before = submitted();
    layer->setTraceView(scene.view);
    EXPECT_FALSE(layer->view().hidden);
    ASSERT_DISPLAY_WAIT(presentsSince(before));
}

// The layer covers the page's screen exactly, its drawable the screen's size in device pixels as
// the page sizes its canvas, through every size change; the next frames are drawn at it. Below the
// minimum width it keeps the page's layout; shorter than the chrome, it hides.
TEST_F(TraceLayerTest, CoversThePagesScreenThroughEverySizeChange)
{
    show();
    ASSERT_DISPLAY_WAIT(presents());
    source.always = true;
    const double scale = window.backingScaleFactor;
    for (const NSSize size : {NSMakeSize(1020, 400), NSMakeSize(1500, 900), NSMakeSize(1080, 580),
                              NSMakeSize(900, 500), NSMakeSize(1500, 900), NSMakeSize(1080, 580)}) {
        [window setContentSize:size];
        EXPECT_TRUE(NSEqualRects(layer->view().frame, expectedFrame()))
            << size.width << " × " << size.height;
        const NSSize frame = expectedFrame().size;
        const auto width = static_cast<std::size_t>(std::round(frame.width * scale));
        const auto height = static_cast<std::size_t>(std::round(frame.height * scale));
        const auto* metal = static_cast<CAMetalLayer*>(layer->view().layer);
        EXPECT_EQ(metal.drawableSize.width, width);
        EXPECT_EQ(metal.drawableSize.height, height);
        ASSERT_DISPLAY_WAIT(drawsWithin(
            [&] {
                const auto stats = layer->statsForTesting();
                return stats.drawnWidth == width && stats.drawnHeight == height;
            },
            2s, "drawn at " + std::to_string(size.width) + " × " + std::to_string(size.height)));
        EXPECT_FALSE(layer->view().hidden);
    }
    [window setContentSize:NSMakeSize(1080, protocol::kToolbarHeight + protocol::kStatusBarHeight)];
    EXPECT_TRUE(layer->view().hidden);
}

// Hidden with the page (a crash, a reload): hidden and paused until the next page has sent its
// trace view, then drawing again at once.
TEST_F(TraceLayerTest, HidesAndPausesWithThePage)
{
    show();
    ASSERT_DISPLAY_WAIT(presents());
    layer->setPageShown(false);
    EXPECT_TRUE(layer->view().hidden);
    pumpUntil([] { return false; }, 100ms);
    const auto callbacks = layer->statsForTesting().callbacks;
    pumpUntil([] { return false; }, 300ms);
    EXPECT_EQ(layer->statsForTesting().callbacks, callbacks) << "the link runs while hidden";
    layer->setPageShown(true);
    EXPECT_TRUE(layer->view().hidden) << "before the new page's trace view";
    const auto before = submitted();
    layer->setTraceView(scene.view);
    EXPECT_FALSE(layer->view().hidden);
    ASSERT_DISPLAY_WAIT(presentsSince(before, "a picture once shown again"));
}

// A window off screen (behind others, minimised, on another space) pauses the link, and the
// clock follows the monotonic time; back on screen, it draws a current frame at once.
TEST_F(TraceLayerTest, PausesWhileItsWindowIsOffScreen)
{
    show();
    ASSERT_DISPLAY_WAIT(presents());
    TraceLayer::setOcclusionForTesting(true);
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSWindowDidChangeOcclusionStateNotification
                      object:window];
    pumpUntil([] { return false; }, 100ms);
    const auto callbacks = layer->statsForTesting().callbacks;
    pumpUntil([] { return false; }, 300ms);
    EXPECT_EQ(layer->statsForTesting().callbacks, callbacks);
    const auto before = submitted();
    TraceLayer::setOcclusionForTesting(false);
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSWindowDidChangeOcclusionStateNotification
                      object:window];
    ASSERT_DISPLAY_WAIT(presentsSince(before, "a picture once back on screen"));
}

// Moved to another screen, the display's lead is measured again, for the new display's link calls
// back at its own lead: meanwhile the clock is the monotonic time, as at the start. Kept at the old
// display's lead, frames on a slower display would judder.
TEST_F(TraceLayerTest, MeasuresTheLeadAgainOnAnotherScreen)
{
    show();
    ASSERT_DISPLAY_WAIT(drawsWithin(
        [&] {
            return layer->statsForTesting().callbacks >
                   static_cast<std::uint64_t>(engine::FramePacing::kLeadCallbacks) + 5;
        },
        3s, "the callbacks that measure the lead"));
    // The display clock, once the lead is known: behind the time now once a frame's moment has
    // passed (before it, a read gets the time now).
    ASSERT_TRUE(pumpUntil(
        [&] {
            const std::uint64_t measured = engine::monotonicNanos();
            return pacing->clock(measured) < measured;
        },
        1s))
        << "the display clock, once the lead is known";
    [NSNotificationCenter.defaultCenter postNotificationName:NSWindowDidChangeScreenNotification
                                                      object:window];
    pumpUntil([] { return false; }, 50ms); // A few callbacks: far fewer than measuring takes.
    const std::uint64_t now = engine::monotonicNanos();
    EXPECT_EQ(pacing->clock(now), now);
}

// A move between a Retina and a 1x display changes the backing scale but no size: the drawable
// follows the new scale.
TEST_F(TraceLayerTest, FollowsTheBackingScaleWithNoSizeChange)
{
    show();
    ASSERT_DISPLAY_WAIT(presents());
    auto* metal = static_cast<CAMetalLayer*>(layer->view().layer);
    const double scale = window.backingScaleFactor;
    const CGSize drawable = metal.drawableSize;
    // As the layer stood on a display of another scale.
    metal.contentsScale = scale * 2.0;
    metal.drawableSize = CGSizeMake(drawable.width * 2.0, drawable.height * 2.0);
    [layer->view() viewDidChangeBackingProperties];
    EXPECT_EQ(metal.contentsScale, scale);
    EXPECT_TRUE(CGSizeEqualToSize(metal.drawableSize, drawable));
}

// Frozen, or in Single after its capture, one frame comes and then none: the layer draws it again
// by itself whenever the picture would change, and not otherwise. Without a redraw it would keep
// a stale picture, or none, until the next frame, which may never come.
TEST_F(TraceLayerTest, DrawsTheLastFrameAgainWhenThePictureChanges)
{
    source.limit = 1;
    show();
    ASSERT_DISPLAY_WAIT(drawsWithin([&] { return source.takes.load() == 1; }, 3s, "the one frame"));
    ASSERT_DISPLAY_WAIT(drawsWithin([&] { return submitted() >= 1; }, 2s, "the one frame drawn"));
    pumpUntil([] { return false; }, 200ms); // Any redraw the start asked for lands.
    const auto idle = layer->statsForTesting().submitted;
    pumpUntil([] { return false; }, 300ms);
    ASSERT_EQ(layer->statsForTesting().submitted, idle) << "nothing to draw again";

    auto before = submitted();
    [window setContentSize:NSMakeSize(1200, 640)];
    ASSERT_DISPLAY_WAIT(presentsSince(before, "a resize"));
    before = submitted();
    [layer->view() viewDidChangeBackingProperties];
    ASSERT_DISPLAY_WAIT(presentsSince(before, "a backing-scale change"));
    TraceView zoomed = scene.view;
    zoomed.view = {.start = 0.25, .end = 0.75};
    before = submitted();
    layer->setTraceView(zoomed);
    ASSERT_DISPLAY_WAIT(presentsSince(before, "a new trace view"));
    TraceLayer::setOcclusionForTesting(true);
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSWindowDidChangeOcclusionStateNotification
                      object:window];
    pumpUntil([] { return false; }, 100ms);
    before = submitted();
    TraceLayer::setOcclusionForTesting(false);
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSWindowDidChangeOcclusionStateNotification
                      object:window];
    ASSERT_DISPLAY_WAIT(presentsSince(before, "back on screen"));

    // A buffer that failed for a while is drawn again at each callback until one succeeds.
    pumpUntil([] { return false; }, 100ms);
    layer->failCommandBuffersForTesting(MTLCommandBufferErrorTimeout);
    [window setContentSize:NSMakeSize(1080, 580)];
    const auto failing = layer->statsForTesting().submitted;
    const std::uint64_t callbacks = layer->statsForTesting().callbacks;
    const auto started = std::chrono::steady_clock::now();
    // Shorter than kFailureSecondsBeforeLost, so the failures alone cannot lose the layer.
    pumpUntil([] { return false; }, 200ms);
    layer->failCommandBuffersForTesting(std::nullopt);
    ASSERT_DISPLAY_WAIT(judge(layer->statsForTesting().submitted >= failing + 3, callbacks, started,
                              "drawn again after failing"));
    EXPECT_FALSE(layer->lost());
    EXPECT_EQ(source.takes.load(), 1);
}

// With the diagnostic switch, each picture the layer draws carries the frame barcode: its number,
// stepping by one on every draw, a redraw with no new frame included, and the link's target time
// it was drawn for. Without it, none.
TEST_F(TraceLayerTest, NumbersEachPictureItDrawsWithTheSwitchOn)
{
    show();
    ASSERT_DISPLAY_WAIT(presents());
    EXPECT_FALSE(layer->statsForTesting().barcode) << "off";

    layer.reset();
    source.frameBarcode = true;
    source.limit = 1;
    source.takes = 0;
    layer = TraceLayer::create(container, above, *pacing, source.hooks());
    show();
    ASSERT_DISPLAY_WAIT(drawsWithin([&] { return source.takes.load() == 1; }, 3s, "the one frame"));
    ASSERT_DISPLAY_WAIT(drawsWithin([&] { return layer->statsForTesting().barcode.has_value(); },
                                    2s, "the one frame drawn"));
    pumpUntil([] { return false; }, 200ms); // Any redraw the start asked for lands.
    const auto first = layer->statsForTesting().barcode;
    ASSERT_TRUE(first);
    const auto before = submitted();
    [window setContentSize:NSMakeSize(1200, 640)];
    ASSERT_DISPLAY_WAIT(presentsSince(before, "a redraw with no new frame"));
    const auto second = layer->statsForTesting().barcode;
    ASSERT_TRUE(second);
    EXPECT_EQ(second->picture, first->picture + 1);
    constexpr std::uint64_t kRange = std::uint64_t{1} << protocol::kBarcodeDataBits;
    const std::uint64_t step = (second->drawnFor + kRange - first->drawnFor) % kRange;
    // Drawn for a later refresh, by less than a second: the link's target time, in the strip's
    // quarter-milliseconds (seconds or another unit would step by nothing or by far more).
    EXPECT_GT(step, 0U);
    EXPECT_LT(step, 1000U * protocol::kBarcodeTicksPerMillisecond);
}

// What reached the screen is counted from Metal's presented times: some frames, never more than
// were submitted. A display that reports no presented times (an idle one) is skipped, saying so.
TEST_F(TraceLayerTest, CountsTheFramesThatReachedTheScreen)
{
    source.always = true;
    show();
    ASSERT_DISPLAY_WAIT(drawsWithin([&] { return submitted() >= 30; }, 3s, "30 pictures"));
    if (!pumpUntil([&] { return layer->statsForTesting().shown.presented > 0; }, 2s)) {
        GTEST_SKIP() << "this display reports no presented times";
    }
    const auto stats = layer->statsForTesting();
    EXPECT_GT(stats.shown.presented, 0U);
    EXPECT_LE(stats.shown.presented + stats.shown.dropped, stats.submitted);
}

// Each shown refresh asks for its frame, and each frame made is drawn.
TEST_F(TraceLayerTest, AsksForFramesAndDrawsEachOneMade)
{
    show();
    ASSERT_DISPLAY_WAIT(
        drawsWithin([&] { return source.takes.load() >= 10; }, 3s, "10 frames taken"));
    EXPECT_GE(source.requests.load(), 10);
    EXPECT_GE(layer->statsForTesting().submitted, 10U);
}

// The frame a refresh draws is asked for after the callback before it, on a timer, not at that
// callback: still on the render thread, as the hook says, and in time for the refresh that draws
// it, so each refresh finds its frame. (When it is asked for, and so how fresh the picture is, is
// FramePacing's: DisplayClock.PlacesEachFrameWhenItIsMade.)
TEST_F(TraceLayerTest, AsksForEachFrameOnTheRenderThreadInTime)
{
    show();
    // Past the first few refreshes (the lead being measured), then over 60 more.
    ASSERT_DISPLAY_WAIT(
        drawsWithin([&] { return source.takes.load() >= 20; }, 3s, "20 frames taken"));
    const int takesBefore = source.takes.load();
    const std::uint64_t callbacksBefore = layer->statsForTesting().callbacks;
    ASSERT_DISPLAY_WAIT(drawsWithin([&] { return source.takes.load() >= takesBefore + 60; }, 4s,
                                    "60 more frames taken"));
    const int takes = source.takes.load() - takesBefore;
    const std::uint64_t callbacks = layer->statsForTesting().callbacks - callbacksBefore;
    const std::scoped_lock lock(source.mutex);
    EXPECT_TRUE(source.requestsOnRenderThread);
    // Each refresh takes the frame it asked for (but the one or two whose take is still to come).
    EXPECT_GE(static_cast<double>(takes), 0.9 * static_cast<double>(callbacks))
        << "refreshes found no frame";
}

// A wait through which the running display link did not call back is read as the display
// stalling, as a CI machine's display does, and not as the layer failing: the tests then skip on
// CI.
TEST_F(TraceLayerTest, ReadsAWaitWithNoCallbacksAsTheDisplayStalling)
{
    show();
    ASSERT_DISPLAY_WAIT(presents());
    layer->stallForTesting(true);
    const testing::DisplayWait wait = drawsWithin([] { return false; }, 300ms, "nothing");
    EXPECT_FALSE(wait.met);
    EXPECT_FALSE(wait.displayRan) << wait.message;
    EXPECT_NE(wait.message.find("the display stalled"), std::string::npos) << wait.message;
}

// A link the layer paused itself (here off screen) is not a stalled display: a wait on it fails,
// on CI too, so a layer that never resumes its link is caught there.
TEST_F(TraceLayerTest, ReadsALinkTheLayerPausedAsNoStall)
{
    show();
    TraceLayer::setOcclusionForTesting(true);
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSWindowDidChangeOcclusionStateNotification
                      object:window];
    ASSERT_TRUE(pumpUntil([&] { return !layer->statsForTesting().linkRunning; }, 1s));
    const testing::DisplayWait wait = drawsWithin([] { return false; }, 300ms, "nothing");
    EXPECT_FALSE(wait.met);
    EXPECT_TRUE(wait.displayRan) << wait.message;
}

class TraceLayerLossTest : public TraceLayerTest, public ::testing::WithParamInterface<long>
{
};

// A removed GPU, revoked access or no permission: native drawing stops for good, said once, and
// nothing is presented after.
TEST_P(TraceLayerLossTest, StopsForGoodWhenTheGpuIsGone)
{
    source.always = true;
    show();
    ASSERT_DISPLAY_WAIT(presents());
    layer->failCommandBuffersForTesting(GetParam());
    ASSERT_DISPLAY_WAIT(drawsWithin([&] { return source.losses.load() == 1; }, 2s, "the loss"));
    EXPECT_TRUE(layer->lost());
    pumpUntil([] { return false; }, 100ms);
    const auto presented = layer->statsForTesting().submitted;
    pumpUntil([] { return false; }, 300ms);
    EXPECT_EQ(layer->statsForTesting().submitted, presented);
    EXPECT_EQ(source.losses.load(), 1);
}

INSTANTIATE_TEST_SUITE_P(PersistentErrors, TraceLayerLossTest,
                         ::testing::Values(11L,
                                           static_cast<long>(MTLCommandBufferErrorAccessRevoked),
                                           static_cast<long>(MTLCommandBufferErrorNotPermitted)),
                         [](const ::testing::TestParamInfo<long>& info) {
                             return info.param == 11L ? std::string("DeviceRemoved")
                                    : info.param == MTLCommandBufferErrorAccessRevoked
                                        ? std::string("AccessRevoked")
                                        : std::string("NotPermitted");
                         });

// Other errors (a GPU timeout, a fault) stop it only once every command buffer has failed for
// kFailureSecondsBeforeLost: a shorter run of failures recovers.
TEST_F(TraceLayerTest, StopsAfterFailingForHalfASecondNotBefore)
{
    source.always = true;
    show();
    ASSERT_DISPLAY_WAIT(presents());
    const auto threshold =
        std::chrono::milliseconds(static_cast<int>(TraceLayer::kFailureSecondsBeforeLost * 1000));
    layer->failCommandBuffersForTesting(MTLCommandBufferErrorTimeout);
    pumpUntil([] { return false; }, threshold * 6 / 10);
    layer->failCommandBuffersForTesting(std::nullopt);
    ASSERT_DISPLAY_WAIT(presents());
    pumpUntil([] { return false; }, threshold * 6 / 10);
    EXPECT_FALSE(layer->lost()) << "failures broken by a clean buffer";

    const auto started = std::chrono::steady_clock::now();
    layer->failCommandBuffersForTesting(MTLCommandBufferErrorTimeout);
    ASSERT_DISPLAY_WAIT(drawsWithin([&] { return layer->lost(); }, threshold * 4, "lost"));
    EXPECT_GE(std::chrono::steady_clock::now() - started, threshold * 9 / 10);
    EXPECT_TRUE(pumpUntil([&] { return source.losses.load() == 1; }, 1s));
}

// Graphics memory refused for a larger screen: native drawing stops for good, said once.
TEST_F(TraceLayerTest, StopsWhenGraphicsMemoryIsRefused)
{
    show();
    ASSERT_DISPLAY_WAIT(presents());
    layer->refuseCoverageTargetsForTesting();
    [window setContentSize:NSMakeSize(1400, 800)];
    ASSERT_DISPLAY_WAIT(drawsWithin([&] { return source.losses.load() == 1; }, 2s, "the loss"));
    EXPECT_TRUE(layer->lost());
}

// Once destroyed, nothing of the editor's is called: the worker and the frame flow it asks may go.
TEST_F(TraceLayerTest, CallsNothingOnceDestroyed)
{
    source.always = true;
    show();
    ASSERT_DISPLAY_WAIT(presents());
    layer.reset();
    const int requests = source.requests.load();
    const int takes = source.takes.load();
    pumpUntil([] { return false; }, 300ms);
    EXPECT_EQ(source.requests.load(), requests);
    EXPECT_EQ(source.takes.load(), takes);
    EXPECT_EQ(container.subviews.count, 1U);
}

} // namespace
} // namespace orizon::editor
