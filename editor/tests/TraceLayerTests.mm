#include "MacPageTesting.h"
#include "engine/MonotonicClock.h"
#include "mac/TraceLayerMac.h"

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
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
    // Each take hands over a new frame (as with a moving trace), or only after a request.
    bool always = false;
    // Frames to hand over in all (none: no end): frozen, or Single after its capture, there is one.
    std::optional<int> limit;
    // The diagnostic switch, as the editor reads it.
    bool frameBarcode = false;
    bool ready = false;
    std::atomic<int> requests{0};
    std::atomic<int> takes{0};
    std::atomic<int> losses{0};

    TraceLayer::Hooks hooks()
    {
        return {.requestFrame =
                    [this] {
                        const std::scoped_lock lock(mutex);
                        ++requests;
                        ready = true;
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

    bool presents(std::chrono::milliseconds within = 2s)
    {
        const auto before = layer->statsForTesting().submitted;
        return pumpUntil([&] { return layer->statsForTesting().submitted > before; }, within);
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
    EXPECT_FALSE(presents(300ms));
    EXPECT_TRUE(layer->view().hidden);
    layer->setTraceView(scene.view);
    EXPECT_FALSE(layer->view().hidden);
    EXPECT_TRUE(presents());
}

// The layer covers the page's screen exactly, its drawable the screen's size in device pixels as
// the page sizes its canvas, through every size change; the next frames are drawn at it. Below the
// minimum width it keeps the page's layout; shorter than the chrome, it hides.
TEST_F(TraceLayerTest, CoversThePagesScreenThroughEverySizeChange)
{
    show();
    ASSERT_TRUE(presents());
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
        EXPECT_TRUE(pumpUntil(
            [&] {
                const auto stats = layer->statsForTesting();
                return stats.drawnWidth == width && stats.drawnHeight == height;
            },
            2s))
            << size.width << " × " << size.height;
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
    ASSERT_TRUE(presents());
    layer->setPageShown(false);
    EXPECT_TRUE(layer->view().hidden);
    pumpUntil([] { return false; }, 100ms);
    const auto callbacks = layer->statsForTesting().callbacks;
    pumpUntil([] { return false; }, 300ms);
    EXPECT_EQ(layer->statsForTesting().callbacks, callbacks) << "the link runs while hidden";
    layer->setPageShown(true);
    EXPECT_TRUE(layer->view().hidden) << "before the new page's trace view";
    layer->setTraceView(scene.view);
    EXPECT_FALSE(layer->view().hidden);
    EXPECT_TRUE(presents());
}

// A window off screen (behind others, minimised, on another space) pauses the link, and the
// clock follows the monotonic time; back on screen, it draws a current frame at once.
TEST_F(TraceLayerTest, PausesWhileItsWindowIsOffScreen)
{
    show();
    ASSERT_TRUE(presents());
    TraceLayer::setOcclusionForTesting(true);
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSWindowDidChangeOcclusionStateNotification
                      object:window];
    pumpUntil([] { return false; }, 100ms);
    const auto callbacks = layer->statsForTesting().callbacks;
    pumpUntil([] { return false; }, 300ms);
    EXPECT_EQ(layer->statsForTesting().callbacks, callbacks);
    TraceLayer::setOcclusionForTesting(false);
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSWindowDidChangeOcclusionStateNotification
                      object:window];
    EXPECT_TRUE(presents());
}

// Moved to another screen, the display's lead is measured again, for the new display's link calls
// back at its own lead: meanwhile the clock is the monotonic time, as at the start. Kept at the old
// display's lead, frames on a slower display would judder.
TEST_F(TraceLayerTest, MeasuresTheLeadAgainOnAnotherScreen)
{
    show();
    ASSERT_TRUE(pumpUntil(
        [&] {
            return layer->statsForTesting().callbacks >
                   static_cast<std::uint64_t>(engine::FramePacing::kLeadCallbacks) + 5;
        },
        3s));
    const std::uint64_t measured = engine::monotonicNanos();
    ASSERT_LT(pacing->clock(measured), measured) << "the display clock, once the lead is known";
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
    ASSERT_TRUE(presents());
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
    ASSERT_TRUE(pumpUntil([&] { return source.takes.load() == 1; }, 3s));
    pumpUntil([] { return false; }, 200ms);
    ASSERT_GE(layer->statsForTesting().submitted, 1U);
    const auto idle = layer->statsForTesting().submitted;
    pumpUntil([] { return false; }, 300ms);
    ASSERT_EQ(layer->statsForTesting().submitted, idle) << "nothing to draw again";

    [window setContentSize:NSMakeSize(1200, 640)];
    EXPECT_TRUE(presents()) << "a resize";
    [layer->view() viewDidChangeBackingProperties];
    EXPECT_TRUE(presents()) << "a backing-scale change";
    TraceView zoomed = scene.view;
    zoomed.view = {.start = 0.25, .end = 0.75};
    layer->setTraceView(zoomed);
    EXPECT_TRUE(presents()) << "a new trace view";
    TraceLayer::setOcclusionForTesting(true);
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSWindowDidChangeOcclusionStateNotification
                      object:window];
    pumpUntil([] { return false; }, 100ms);
    TraceLayer::setOcclusionForTesting(false);
    [NSNotificationCenter.defaultCenter
        postNotificationName:NSWindowDidChangeOcclusionStateNotification
                      object:window];
    EXPECT_TRUE(presents()) << "back on screen";

    // A buffer that failed for a while is drawn again at each callback until one succeeds.
    pumpUntil([] { return false; }, 100ms);
    layer->failCommandBuffersForTesting(MTLCommandBufferErrorTimeout);
    [window setContentSize:NSMakeSize(1080, 580)];
    const auto failing = layer->statsForTesting().submitted;
    pumpUntil([] { return false; }, 200ms);
    layer->failCommandBuffersForTesting(std::nullopt);
    EXPECT_GE(layer->statsForTesting().submitted, failing + 3) << "drawn again after failing";
    EXPECT_FALSE(layer->lost());
    EXPECT_EQ(source.takes.load(), 1);
}

// With the diagnostic switch, each picture the layer draws carries the frame barcode: its number,
// stepping by one on every draw, a redraw with no new frame included, and the link's target time
// it was drawn for. Without it, none.
TEST_F(TraceLayerTest, NumbersEachPictureItDrawsWithTheSwitchOn)
{
    show();
    ASSERT_TRUE(presents());
    EXPECT_FALSE(layer->statsForTesting().barcode) << "off";

    layer.reset();
    source.frameBarcode = true;
    source.limit = 1;
    source.takes = 0;
    layer = TraceLayer::create(container, above, *pacing, source.hooks());
    show();
    ASSERT_TRUE(pumpUntil([&] { return source.takes.load() == 1; }, 3s));
    pumpUntil([] { return false; }, 200ms);
    const auto first = layer->statsForTesting().barcode;
    ASSERT_TRUE(first);
    [window setContentSize:NSMakeSize(1200, 640)];
    ASSERT_TRUE(presents()) << "a redraw with no new frame";
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
    ASSERT_TRUE(pumpUntil([&] { return layer->statsForTesting().submitted >= 30; }, 3s));
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
    ASSERT_TRUE(pumpUntil([&] { return source.takes.load() >= 10; }, 3s));
    EXPECT_GE(source.requests.load(), 10);
    EXPECT_GE(layer->statsForTesting().submitted, 10U);
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
    ASSERT_TRUE(presents());
    layer->failCommandBuffersForTesting(GetParam());
    ASSERT_TRUE(pumpUntil([&] { return source.losses.load() == 1; }, 2s));
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
    ASSERT_TRUE(presents());
    const auto threshold =
        std::chrono::milliseconds(static_cast<int>(TraceLayer::kFailureSecondsBeforeLost * 1000));
    layer->failCommandBuffersForTesting(MTLCommandBufferErrorTimeout);
    pumpUntil([] { return false; }, threshold * 6 / 10);
    layer->failCommandBuffersForTesting(std::nullopt);
    EXPECT_TRUE(presents());
    pumpUntil([] { return false; }, threshold * 6 / 10);
    EXPECT_FALSE(layer->lost()) << "failures broken by a clean buffer";

    const auto started = std::chrono::steady_clock::now();
    layer->failCommandBuffersForTesting(MTLCommandBufferErrorTimeout);
    ASSERT_TRUE(pumpUntil([&] { return layer->lost(); }, threshold * 4));
    EXPECT_GE(std::chrono::steady_clock::now() - started, threshold * 9 / 10);
    EXPECT_TRUE(pumpUntil([&] { return source.losses.load() == 1; }, 1s));
}

// Graphics memory refused for a larger screen: native drawing stops for good, said once.
TEST_F(TraceLayerTest, StopsWhenGraphicsMemoryIsRefused)
{
    show();
    ASSERT_TRUE(presents());
    layer->refuseCoverageTargetsForTesting();
    [window setContentSize:NSMakeSize(1400, 800)];
    EXPECT_TRUE(pumpUntil([&] { return source.losses.load() == 1; }, 2s));
    EXPECT_TRUE(layer->lost());
}

// Once destroyed, nothing of the editor's is called: the worker and the frame flow it asks may go.
TEST_F(TraceLayerTest, CallsNothingOnceDestroyed)
{
    source.always = true;
    show();
    ASSERT_TRUE(presents());
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
