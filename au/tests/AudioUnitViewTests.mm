#include "AudioUnit.h"
#include "AudioUnitView.h"
#include "AudioUnitViewTesting.h"
#include "MacPageTesting.h"
#include "editor/EditorCore.h"
#include "editor/WebView.h"
#include "engine/EngineRegistry.h"
#include "engine/InstanceRegistry.h"
#include "engine/Version.h"

#import <AudioToolbox/AUCocoaUIView.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace orizon::au {
namespace {

using editor::EditorCore;
using testing::evaluate;
using testing::kPageTimeout;
using testing::pumpUntil;

// The Audio Unit, registered in this process the way a host finds an installed one.
AudioComponent component()
{
    static AudioComponent registered = ausdk::AUBaseFactory<AudioUnit>::Register(
        kComponentDescription.componentType, kComponentDescription.componentSubType,
        kComponentDescription.componentManufacturer, CFSTR("Orizon Audio: Sineless"), 1);
    return registered;
}

WKWebView* webViewOf(NSView* view)
{
    EditorCore* core = editorCoreOf(view);
    if (core == nullptr || core->webView() == nullptr) {
        return nil;
    }
    return (__bridge WKWebView*)core->webView()->nativeView();
}

class AudioUnitViewTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { [NSApplication sharedApplication]; }

    // An instance as a host makes one. Not initialised: a host may open the view before it
    // initialises the Audio Unit.
    void SetUp() override { ASSERT_EQ(AudioComponentInstanceNew(component(), &unit), noErr); }
    void TearDown() override
    {
        if (unit != nullptr) {
            AudioComponentInstanceDispose(unit);
        }
    }

    // The instance's engine, found by its token as the view finds it.
    [[nodiscard]] std::shared_ptr<engine::Engine> engineOf(::AudioUnit of) const
    {
        std::int64_t token = 0;
        UInt32 size = sizeof token;
        EXPECT_EQ(AudioUnitGetProperty(of, kEngineTokenProperty, kAudioUnitScope_Global, 0, &token,
                                       &size),
                  noErr);
        return engine::EngineRegistry::find(token);
    }

    // What a host does when it opens the window: make the factory, ask it for a view. The
    // factory's view is autoreleased, and a host's run loop drains it; this process has no pool
    // of its own, so the test's reference is the only one left.
    static NSView* openView(::AudioUnit of, NSSize asked = NSMakeSize(400, 300))
    {
        NSView* view = nil;
        @autoreleasepool {
            Class factoryClass = NSClassFromString(@(viewFactoryClassName()));
            id<AUCocoaUIBase> factory = [[factoryClass alloc] init];
            view = [factory uiViewForAudioUnit:of withSize:asked];
        }
        return view;
    }

    // A host window holding `view`.
    static NSWindow* windowFor(NSView* view)
    {
        NSWindow* window = [[NSWindow alloc] initWithContentRect:view.frame
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        [window.contentView addSubview:view];
        return window;
    }

    // Runs what the main queue holds now: anything the view put off to the next turn of the run
    // loop has run when this returns.
    static void runQueuedWork()
    {
        auto reached = std::make_shared<bool>(false);
        dispatch_async(dispatch_get_main_queue(), ^{
          *reached = true;
        });
        pumpUntil([reached] { return *reached; }, std::chrono::seconds(5));
    }

    // Puts `view` in Multi, stacked, with `count` tracks in its group: the others' engines, which
    // removeTracks takes out of the group again.
    [[nodiscard]] std::vector<std::shared_ptr<engine::Engine>> stackTracks(int count) const
    {
        const auto own = engineOf(unit);
        own->settings().set(protocol::SettingId::kDisplayMode,
                            static_cast<double>(protocol::DisplayMode::kMulti));
        own->settings().set(protocol::SettingId::kViewLayout,
                            static_cast<double>(protocol::ViewLayout::kStacked));
        std::vector<std::shared_ptr<engine::Engine>> others;
        for (int track = 1; track < count; ++track) {
            others.push_back(std::make_shared<engine::Engine>());
            engine::InstanceRegistry::shared().add(others.back());
        }
        return others;
    }
    static void removeTracks(const std::vector<std::shared_ptr<engine::Engine>>& others)
    {
        for (const auto& engine : others) {
            engine::InstanceRegistry::shared().remove(*engine);
        }
    }

    // Lays `view` out as Logic does when it hosts Audio Units out of process: with Auto Layout,
    // pinned to its window's content at the corner and, at a low priority, to its size.
    static void pinWithAutoLayout(NSView* view)
    {
        NSView* container = view.superview;
        view.translatesAutoresizingMaskIntoConstraints = NO;
        NSArray<NSLayoutConstraint*>* fill = @[
            [view.widthAnchor constraintEqualToAnchor:container.widthAnchor],
            [view.heightAnchor constraintEqualToAnchor:container.heightAnchor],
        ];
        for (NSLayoutConstraint* constraint : fill) {
            // Below a view's own size, and below its default hugging (DefaultLow): a host's
            // constraints may rank anywhere under its compression resistance.
            constraint.priority = NSLayoutPriorityDefaultLow - 1;
        }
        [NSLayoutConstraint
            activateConstraints:[fill arrayByAddingObjectsFromArray:@[
                [view.leadingAnchor constraintEqualToAnchor:container.leadingAnchor],
                [view.topAnchor constraintEqualToAnchor:container.topAnchor],
            ]]];
    }
    // A layout pass of the host's, which applies its constraints again.
    static void layOut(NSView* view)
    {
        view.superview.needsLayout = YES;
        [view.superview layoutSubtreeIfNeeded];
    }

    // What a host does when it closes the window: the view leaves it, which closes the editor.
    static void closeWindow(NSWindow* window, NSView* view)
    {
        [view removeFromSuperview];
        [window close];
    }

    ::AudioUnit unit = nullptr;
};

// A host learns where the view comes from through kAudioUnitProperty_CocoaUI: the factory class
// by name, conforming to Apple's protocol at the version hosts expect.
TEST_F(AudioUnitViewTest, AdvertisesItsViewFactory)
{
    UInt32 size = 0;
    Boolean writable = true;
    ASSERT_EQ(AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
                                       &size, &writable),
              noErr);
    EXPECT_EQ(size, sizeof(AudioUnitCocoaViewInfo));
    EXPECT_FALSE(writable);

    Class factoryClass = NSClassFromString(@(viewFactoryClassName()));
    ASSERT_NE(factoryClass, nil);
    EXPECT_TRUE([factoryClass conformsToProtocol:@protocol(AUCocoaUIBase)]);
    id<AUCocoaUIBase> factory = [[factoryClass alloc] init];
    EXPECT_EQ([factory interfaceVersion], 0U);
}

// The factory's name carries the version, so another version loaded in the same host (an
// installed release beside a newer one) never shares the class.
TEST(AudioUnitView, NamesTheFactoryForThisVersion)
{
    std::string version = engine::kVersionString;
    std::ranges::replace(version, '.', '_');
    const std::string name = viewFactoryClassName();
    EXPECT_NE(name.find("Sineless"), std::string::npos) << name;
    EXPECT_NE(name.find(version), std::string::npos) << name;
}

// A host asks the Audio Unit where its view comes from, loads that bundle and asks it for the
// class by name. This test runs from a bundle, as the component does: the answer is the bundle
// that holds this binary, and the class it names there makes the editor's view. The host releases
// both values.
TEST_F(AudioUnitViewTest, OffersItsViewToTheHost)
{
    AudioUnitCocoaViewInfo info{};
    UInt32 size = sizeof info;
    ASSERT_EQ(AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
                                   &info, &size),
              noErr);
    EXPECT_EQ(size, sizeof info);

    auto* location = (__bridge NSURL*)info.mCocoaAUViewBundleLocation;
    EXPECT_EQ(std::string(location.URLByStandardizingPath.path.UTF8String),
              std::string(NSBundle.mainBundle.bundleURL.URLByStandardizingPath.path.UTF8String));
    NSBundle* bundle = [NSBundle bundleWithURL:location];
    ASSERT_NE(bundle, nil);
    Class factoryClass = [bundle classNamed:(__bridge NSString*)info.mCocoaAUViewClass[0]];
    ASSERT_NE(factoryClass, nil);
    ASSERT_TRUE([factoryClass conformsToProtocol:@protocol(AUCocoaUIBase)]);
    NSView* view = nil;
    @autoreleasepool {
        id<AUCocoaUIBase> factory = [[factoryClass alloc] init];
        view = [factory uiViewForAudioUnit:unit withSize:NSMakeSize(400, 300)];
    }
    EXPECT_NE(editorCoreOf(view), nullptr) << "the factory makes the editor's view";

    CFRelease(info.mCocoaAUViewBundleLocation);
    CFRelease(info.mCocoaAUViewClass[0]);
}

// A binary outside any bundle could not have its view loaded by a host, so none is offered.
TEST(AudioUnitView, OffersNoViewOutsideABundle)
{
    AudioUnitCocoaViewInfo info{};
    EXPECT_EQ(copyCocoaViewInfo("/usr/local/bin/au_view_tests", info),
              kAudioUnitErr_InvalidProperty);
}

// What a host gets from kAudioUnitProperty_CocoaUI for an installed component: the component's
// bundle, a directory, and a class name it resolves to a factory that makes the editor's view.
// The host releases both values.
TEST_F(AudioUnitViewTest, TellsTheHostWhereToFindTheView)
{
    const std::string component = "/Users/a/Library/Audio/Plug-Ins/Components/Sineless.component";
    AudioUnitCocoaViewInfo info{};
    ASSERT_EQ(copyCocoaViewInfo(component + "/Contents/MacOS/Sineless", info), noErr);

    CFStringRef path =
        CFURLCopyFileSystemPath(info.mCocoaAUViewBundleLocation, kCFURLPOSIXPathStyle);
    EXPECT_EQ(std::string([(__bridge NSString*)path UTF8String]), component);
    EXPECT_TRUE(CFURLHasDirectoryPath(info.mCocoaAUViewBundleLocation));
    CFRelease(path);

    Class factoryClass = NSClassFromString((__bridge NSString*)info.mCocoaAUViewClass[0]);
    ASSERT_NE(factoryClass, nil);
    ASSERT_TRUE([factoryClass conformsToProtocol:@protocol(AUCocoaUIBase)]);
    NSView* view = nil;
    @autoreleasepool {
        id<AUCocoaUIBase> factory = [[factoryClass alloc] init];
        view = [factory uiViewForAudioUnit:unit withSize:NSMakeSize(400, 300)];
    }
    EXPECT_NE(editorCoreOf(view), nullptr) << "the factory makes the editor's view";

    CFRelease(info.mCocoaAUViewBundleLocation);
    CFRelease(info.mCocoaAUViewClass[0]);
}

// The bundle is the one that holds the Audio Unit's binary, wherever it is installed.
TEST(AudioUnitView, FindsTheBundleThatHoldsTheBinary)
{
    EXPECT_EQ(bundlePathFor("/Users/a/Library/Audio/Plug-Ins/Components/Sineless.component/"
                            "Contents/MacOS/Sineless"),
              "/Users/a/Library/Audio/Plug-Ins/Components/Sineless.component");
    EXPECT_EQ(bundlePathFor("/Library/Audio/Plug-Ins/Components/Sineless 2.component/Contents/"
                            "MacOS/Sineless"),
              "/Library/Audio/Plug-Ins/Components/Sineless 2.component");
    EXPECT_EQ(bundlePathFor("/usr/local/bin/au_view_tests"), std::nullopt);
    EXPECT_EQ(bundlePathFor("/Contents/MacOS/Sineless"), std::nullopt);
    EXPECT_EQ(bundlePathFor("/a/Sineless.component/Contents/MacOS/"), std::nullopt);
    EXPECT_EQ(bundlePathFor(""), std::nullopt);
}

// Each opening of the window gets a new view, at the editor's own size: the size a host asks for
// is only a hint, and hosts send nonsense.
TEST_F(AudioUnitViewTest, GivesANewViewAtTheEditorsSizeEachTime)
{
    NSView* first = openView(unit, NSMakeSize(10, 10));
    NSView* second = openView(unit, NSMakeSize(0, 0));
    NSView* third = openView(unit, NSMakeSize(5000, 4000));
    ASSERT_NE(first, nil);
    EXPECT_NE(first, second);
    EXPECT_NE(second, third);
    for (NSView* view : {first, second, third}) {
        EXPECT_EQ(view.frame.size.width, EditorCore::kDefaultWidth);
        EXPECT_EQ(view.frame.size.height, EditorCore::kDefaultHeight);
        EXPECT_NE(view.autoresizingMask & NSViewWidthSizable, 0U) << "resizable";
        EXPECT_NE(view.autoresizingMask & NSViewHeightSizable, 0U) << "resizable";
    }
}

// The view shows this instance: the page draws frames from its engine and follows its settings.
TEST_F(AudioUnitViewTest, ShowsTheInstancesEngine)
{
    const auto engine = engineOf(unit);
    ASSERT_NE(engine, nullptr);
    engine->settings().set(protocol::SettingId::kTriggerMode, 2.0); // Free: a frame per request.
    std::vector<float> tone(4800, 0.5F);
    std::array<const float*, 2> channels{tone.data(), tone.data()};
    engine->captureRing().write(channels.data(), 2, static_cast<int>(tone.size()));

    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    WKWebView* webView = webViewOf(view);
    ASSERT_NE(webView, nil);
    EXPECT_TRUE(editorCoreOf(view)->engineAvailable());
    ASSERT_TRUE(pumpUntil(
        [&] { return evaluate(webView, @"String(window.orizonDebug?.frameCount ?? 0)") != "0"; },
        kPageTimeout))
        << "the page draws a frame from the engine";

    engine::Settings changed;
    changed.set(protocol::SettingId::kTriggerLevel, 0.5);
    engine->settings().replace(changed);
    EXPECT_TRUE(pumpUntil(
        [&] {
            return evaluate(webView, @"String(window.orizonDebug?.lastState?.name ?? '')") ==
                   "triggerLevel";
        },
        kPageTimeout))
        << "a change to the instance's settings reaches the page";
    closeWindow(window, view);
}

// Given an Audio Unit that is not Sineless (so no engine to find), the view explains that it has
// no data rather than waiting forever.
TEST_F(AudioUnitViewTest, ExplainsWhenItFindsNoEngine)
{
    AudioComponentDescription delay{.componentType = kAudioUnitType_Effect,
                                    .componentSubType = kAudioUnitSubType_Delay,
                                    .componentManufacturer = kAudioUnitManufacturer_Apple,
                                    .componentFlags = 0,
                                    .componentFlagsMask = 0};
    AudioComponent appleDelay = AudioComponentFindNext(nullptr, &delay);
    ASSERT_NE(appleDelay, nullptr);
    ::AudioUnit other = nullptr;
    ASSERT_EQ(AudioComponentInstanceNew(appleDelay, &other), noErr);

    NSView* view = openView(other);
    NSWindow* window = windowFor(view);
    EXPECT_FALSE(editorCoreOf(view)->engineAvailable());
    EXPECT_FALSE(editorCoreOf(view)->workerRunning());
    WKWebView* webView = webViewOf(view);
    ASSERT_NE(webView, nil);
    EXPECT_TRUE(pumpUntil(
        [&] {
            return evaluate(webView, @"String(window.orizonDebug?.error?.code ?? '')") ==
                   "engineUnavailable";
        },
        kPageTimeout));
    closeWindow(window, view);
    AudioComponentInstanceDispose(other);
}

// The editor runs (page, analysis worker) only while the view is in a window: it starts when the
// host shows the view, stops when the view leaves its window, and starts again in another.
TEST_F(AudioUnitViewTest, RunsTheEditorOnlyWhileInAWindow)
{
    NSView* view = openView(unit);
    EXPECT_FALSE(editorCoreOf(view)->workerRunning()) << "not before it is shown";
    EXPECT_EQ(webViewOf(view), nil);

    NSWindow* window = windowFor(view);
    EXPECT_TRUE(editorCoreOf(view)->workerRunning());
    EXPECT_NE(webViewOf(view), nil);

    [view removeFromSuperview];
    EXPECT_FALSE(editorCoreOf(view)->workerRunning()) << "stopped when it leaves";
    EXPECT_EQ(webViewOf(view), nil);

    NSWindow* another = windowFor(view);
    EXPECT_TRUE(editorCoreOf(view)->workerRunning()) << "started again in a new window";
    EXPECT_NE(webViewOf(view), nil);

    [view removeFromSuperview];
    [another close];
    [window close];
}

// Hosts make a new view each time the window opens and release it when it closes: each view is
// freed with its editor, however often the window opens.
TEST_F(AudioUnitViewTest, FreesEachViewTheHostReleases)
{
    for (int cycle = 0; cycle < 10; ++cycle) {
        __weak NSView* released = nil;
        @autoreleasepool {
            NSView* view = openView(unit);
            released = view;
            NSWindow* window = windowFor(view);
            ASSERT_TRUE(editorCoreOf(view)->workerRunning());
            [window close];
        }
        EXPECT_TRUE(pumpUntil([&] { return released == nil; }, std::chrono::seconds(5)))
            << "cycle " << cycle;
    }
}

// The host resizes the view (the user drags the window); the page takes the new size.
TEST_F(AudioUnitViewTest, FollowsTheHostsResizes)
{
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    for (const NSSize size : {NSMakeSize(1200, 700), NSMakeSize(EditorCore::kMinimumWidth, 300)}) {
        [view setFrameSize:size];
        WKWebView* webView = webViewOf(view);
        ASSERT_NE(webView, nil);
        EXPECT_EQ(webView.frame.size.width, size.width);
        EXPECT_EQ(webView.frame.size.height, size.height);
    }
    closeWindow(window, view);
}

// A host may make the window smaller than the editor's minimum (the user drags it, or the host
// restores a saved size): the view takes that size for the moment, then returns to the minimum,
// which the host follows. Never from inside the host's resize, which could loop with it.
TEST_F(AudioUnitViewTest, ReturnsToItsMinimumAfterTheHostMakesItSmaller)
{
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    const NSSize minimum = NSMakeSize(EditorCore::kMinimumWidth, EditorCore::kMinimumHeight);
    for (const NSSize small :
         {NSMakeSize(0, 0), NSMakeSize(300, 200), NSMakeSize(minimum.width - 1, 600),
          NSMakeSize(900, minimum.height - 1)}) {
        [view setFrameSize:small];
        EXPECT_TRUE(NSEqualSizes(view.frame.size, small)) << "not inside the host's resize";
        runQueuedWork();
        const NSSize expected = NSMakeSize(std::max(small.width, minimum.width),
                                           std::max(small.height, minimum.height));
        EXPECT_TRUE(NSEqualSizes(view.frame.size, expected))
            << view.frame.size.width << "x" << view.frame.size.height;
        WKWebView* webView = webViewOf(view);
        ASSERT_NE(webView, nil);
        EXPECT_TRUE(NSEqualSizes(webView.frame.size, expected)) << "the page has the room";
    }
    closeWindow(window, view);
}

// The user drags the window smaller than the minimum: the view follows the drag without fighting
// it, and returns to the minimum when the drag ends. AppKit tells every view in the window when a
// live resize starts and ends.
TEST_F(AudioUnitViewTest, ReturnsToItsMinimumWhenTheUserStopsDragging)
{
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    // Twice to the same smallest size, as a window stops at its own minimum each time: every drag
    // is the user's, never a size the host insists on.
    for (int drag = 0; drag < 2; ++drag) {
        SCOPED_TRACE(drag);
        [view viewWillStartLiveResize];
        for (const NSSize dragged : {NSMakeSize(600, 220), NSMakeSize(500, 200)}) {
            [view setFrameSize:dragged];
            runQueuedWork();
            EXPECT_TRUE(NSEqualSizes(view.frame.size, dragged)) << "follows the drag";
        }
        [view viewDidEndLiveResize];
        runQueuedWork();
        EXPECT_EQ(view.frame.size.width, EditorCore::kMinimumWidth);
        EXPECT_EQ(view.frame.size.height, EditorCore::kMinimumHeight);
    }
    closeWindow(window, view);
}

// A host that sets the same small size again after the view returned to its minimum keeps it:
// the view does not fight the host over its size.
TEST_F(AudioUnitViewTest, LeavesASmallSizeTheHostInsistsOn)
{
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    const NSSize small = NSMakeSize(300, 200);
    [view setFrameSize:small];
    runQueuedWork();
    ASSERT_FALSE(NSEqualSizes(view.frame.size, small)) << "returned to the minimum once";
    [view setFrameSize:small];
    runQueuedWork();
    EXPECT_TRUE(NSEqualSizes(view.frame.size, small));

    [view setFrameSize:NSMakeSize(250, 200)]; // Another size: the minimum applies again.
    runQueuedWork();
    EXPECT_EQ(view.frame.size.width, EditorCore::kMinimumWidth);
    closeWindow(window, view);
}

// With many stacked lanes the editor needs more height than the window has: the view grows, which
// the host follows, and the page gets the room.
TEST_F(AudioUnitViewTest, GrowsForStackedLanes)
{
    const auto own = engineOf(unit);
    own->settings().set(protocol::SettingId::kDisplayMode,
                        static_cast<double>(protocol::DisplayMode::kMulti));
    own->settings().set(protocol::SettingId::kViewLayout,
                        static_cast<double>(protocol::ViewLayout::kStacked));
    std::vector<std::shared_ptr<engine::Engine>> others; // Fifteen more tracks in its group.
    for (int track = 1; track < 16; ++track) {
        others.push_back(std::make_shared<engine::Engine>());
        engine::InstanceRegistry::shared().add(others.back());
    }

    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    const int wanted = EditorCore::kChromeHeight + 16 * EditorCore::kMinimumLaneHeight;
    EXPECT_TRUE(pumpUntil([&] { return view.frame.size.height == wanted; }, kPageTimeout))
        << "the view grows to the height sixteen lanes need";
    WKWebView* webView = webViewOf(view);
    ASSERT_NE(webView, nil);
    EXPECT_EQ(webView.frame.size.height, wanted) << "the page has the room";

    [view setFrameSize:NSMakeSize(view.frame.size.width, 600)];
    runQueuedWork();
    EXPECT_EQ(view.frame.size.height, wanted) << "the lanes' height is the minimum";

    closeWindow(window, view);
    for (const auto& engine : others) {
        engine::InstanceRegistry::shared().remove(*engine);
    }
}

// Logic hosts Audio Units out of process, where the view is laid out with Auto Layout: pinned to a
// container the size the view had when it opened, and the container's constraints applied again on
// every layout pass. The view still keeps the height its stacked lanes need, rather than being set
// back to the container's height with the lanes squeezed into it.
TEST_F(AudioUnitViewTest, GrowsForStackedLanesInAHostThatUsesAutoLayout)
{
    const auto others = stackTracks(16);
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    pinWithAutoLayout(view);
    const int wanted = EditorCore::kChromeHeight + 16 * EditorCore::kMinimumLaneHeight;
    EXPECT_TRUE(pumpUntil(
        [&] {
            layOut(view);
            return view.frame.size.height == wanted;
        },
        kPageTimeout))
        << "the view has the height sixteen lanes need, not " << view.frame.size.height;
    runQueuedWork();
    layOut(view);
    EXPECT_EQ(view.frame.size.height, wanted) << "and keeps it through the host's layout";
    WKWebView* webView = webViewOf(view);
    ASSERT_NE(webView, nil);
    EXPECT_EQ(webView.frame.size.height, wanted) << "the page has the room";

    closeWindow(window, view);
    removeTracks(others);
}

// In a host that lays the view out with Auto Layout, a window made larger than the editor's
// minimum is filled: the editor's minimum holds the view no smaller, never no larger.
TEST_F(AudioUnitViewTest, FillsALargerWindowInAHostThatUsesAutoLayout)
{
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    pinWithAutoLayout(view);
    const NSSize larger =
        NSMakeSize(EditorCore::kDefaultWidth + 300, EditorCore::kDefaultHeight + 400);
    [window setContentSize:larger];
    layOut(view);
    EXPECT_TRUE(NSEqualSizes(view.frame.size, larger))
        << view.frame.size.width << "x" << view.frame.size.height;
    WKWebView* webView = webViewOf(view);
    ASSERT_NE(webView, nil);
    EXPECT_TRUE(NSEqualSizes(webView.frame.size, larger)) << "the page has the room";
    closeWindow(window, view);
}

// In a host that lays the view out with Auto Layout, the editor's minimum holds however it changed:
// the lanes grew while the window had room, so the editor asked for none, and the window is then
// made smaller. The view keeps the height the lanes need.
TEST_F(AudioUnitViewTest, KeepsLanesThatGrewInRoomInAHostThatUsesAutoLayout)
{
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    pinWithAutoLayout(view);
    const int wanted = EditorCore::kChromeHeight + 16 * EditorCore::kMinimumLaneHeight;
    [window setContentSize:NSMakeSize(EditorCore::kDefaultWidth, wanted + 100)];
    layOut(view);
    runQueuedWork();
    const auto others = stackTracks(16); // Tracks added while the editor is open.
    ASSERT_TRUE(pumpUntil(
        [&] {
            layOut(view);
            return view.fittingSize.height == wanted;
        },
        kPageTimeout))
        << "Auto Layout has the lanes' height as the view's minimum, not "
        << view.fittingSize.height;
    ASSERT_EQ(view.frame.size.height, wanted + 100) << "the window had room: none was asked for";

    [window setContentSize:NSMakeSize(EditorCore::kDefaultWidth, wanted - 100)];
    EXPECT_TRUE(pumpUntil(
        [&] {
            layOut(view);
            return view.frame.size.height == wanted;
        },
        kPageTimeout))
        << "the view keeps the height the lanes need, not " << view.frame.size.height;
    closeWindow(window, view);
    removeTracks(others);
}

// In a host that lays the view out with Auto Layout, the view follows the window down to the
// editor's minimum as it is now: after the stacked lanes are no longer shown, a smaller window is
// filled rather than cut off at the lanes' old height.
TEST_F(AudioUnitViewTest, FollowsASmallerWindowOnceTheLanesAreGoneInAHostThatUsesAutoLayout)
{
    const auto others = stackTracks(16);
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    pinWithAutoLayout(view);
    const int wanted = EditorCore::kChromeHeight + 16 * EditorCore::kMinimumLaneHeight;
    ASSERT_TRUE(pumpUntil(
        [&] {
            layOut(view);
            return view.frame.size.height == wanted;
        },
        kPageTimeout));

    engineOf(unit)->settings().set(protocol::SettingId::kDisplayMode,
                                   static_cast<double>(protocol::DisplayMode::kSingle));
    const NSSize smaller = NSMakeSize(EditorCore::kDefaultWidth, EditorCore::kDefaultHeight);
    [window setContentSize:smaller];
    EXPECT_TRUE(pumpUntil(
        [&] {
            layOut(view);
            return NSEqualSizes(view.frame.size, smaller);
        },
        kPageTimeout))
        << view.frame.size.width << "x" << view.frame.size.height;
    closeWindow(window, view);
    removeTracks(others);
}

// Dragging the editor's resize grip resizes the view, which the host's window follows: in Logic,
// and in REAPER for Audio Units, the only way to resize the editor. Never smaller than the
// editor's minimum.
TEST_F(AudioUnitViewTest, ResizesToTheSizeTheGripIsDraggedTo)
{
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    WKWebView* webView = webViewOf(view);
    ASSERT_NE(webView, nil);
    ASSERT_TRUE(pumpUntil([&] { return testing::hasSnapshot(webView); }, kPageTimeout));
    const auto drag = [&](int width, int height) {
        evaluate(webView, [NSString stringWithFormat:@"window.webkit.messageHandlers.orizon."
                                                     @"postMessage(JSON.stringify({type: "
                                                     @"'resize', width: %d, height: %d})); ''",
                                                     width, height]);
    };

    const NSSize larger =
        NSMakeSize(EditorCore::kDefaultWidth + 300, EditorCore::kDefaultHeight + 200);
    drag(static_cast<int>(larger.width), static_cast<int>(larger.height));
    EXPECT_TRUE(pumpUntil([&] { return NSEqualSizes(view.frame.size, larger); }, kPageTimeout))
        << view.frame.size.width << "x" << view.frame.size.height;
    EXPECT_TRUE(NSEqualSizes(webView.frame.size, larger)) << "the page has the room";

    drag(300, 200);
    const NSSize minimum =
        NSMakeSize(EditorCore::kMinimumWidth, editorCoreOf(view)->minimumHeight());
    EXPECT_TRUE(pumpUntil([&] { return NSEqualSizes(view.frame.size, minimum); }, kPageTimeout))
        << view.frame.size.width << "x" << view.frame.size.height;
    closeWindow(window, view);
}

// In a host that lays the view out with Auto Layout, as Logic does, the size the grip was dragged
// to holds while the host's window catches up, rather than being set back to the window's size.
TEST_F(AudioUnitViewTest, KeepsTheSizeTheGripIsDraggedToInAHostThatUsesAutoLayout)
{
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    pinWithAutoLayout(view);
    WKWebView* webView = webViewOf(view);
    ASSERT_NE(webView, nil);
    ASSERT_TRUE(pumpUntil([&] { return testing::hasSnapshot(webView); }, kPageTimeout));
    const NSSize larger =
        NSMakeSize(EditorCore::kDefaultWidth + 300, EditorCore::kDefaultHeight + 200);
    evaluate(webView, [NSString stringWithFormat:@"window.webkit.messageHandlers.orizon."
                                                 @"postMessage(JSON.stringify({type: 'resize', "
                                                 @"width: %d, height: %d})); ''",
                                                 static_cast<int>(larger.width),
                                                 static_cast<int>(larger.height)]);
    EXPECT_TRUE(pumpUntil(
        [&] {
            layOut(view);
            return NSEqualSizes(view.frame.size, larger);
        },
        kPageTimeout))
        << view.frame.size.width << "x" << view.frame.size.height;
    for (int pass = 0; pass < 3; ++pass) {
        layOut(view);
        runQueuedWork();
    }
    EXPECT_TRUE(NSEqualSizes(view.frame.size, larger)) << "through the host's layout passes";
    closeWindow(window, view);
}

// The view opens again at the size it was left at while the plugin stays loaded, and at the
// editor's minimum, not cropped, after a host insisted on a smaller one.
TEST_F(AudioUnitViewTest, OpensAgainAtTheSizeItWasLeftAt)
{
    const NSSize left =
        NSMakeSize(EditorCore::kDefaultWidth + 400, EditorCore::kDefaultHeight + 300);
    {
        NSView* view = openView(unit);
        NSWindow* window = windowFor(view);
        [view setFrameSize:left];
        closeWindow(window, view);
    }
    NSView* reopened = openView(unit);
    EXPECT_TRUE(NSEqualSizes(reopened.frame.size, left))
        << reopened.frame.size.width << "x" << reopened.frame.size.height;

    NSWindow* window = windowFor(reopened);
    const NSSize small = NSMakeSize(300, 200);
    [reopened setFrameSize:small];
    runQueuedWork();
    [reopened setFrameSize:small]; // The host insists.
    runQueuedWork();
    ASSERT_TRUE(NSEqualSizes(reopened.frame.size, small));
    closeWindow(window, reopened);
    NSView* again = openView(unit);
    EXPECT_EQ(again.frame.size.width, EditorCore::kMinimumWidth);
    EXPECT_EQ(again.frame.size.height, EditorCore::kMinimumHeight);
}

// A host may dispose of the Audio Unit before it closes the view: the view keeps what it shows
// and closes cleanly afterwards.
TEST_F(AudioUnitViewTest, ClosesCleanlyAfterTheAudioUnitIsDisposed)
{
    // Only a weak reference here: after the dispose, the engine is alive only if the view holds it.
    const std::weak_ptr<engine::Engine> held = engineOf(unit);
    held.lock()->settings().set(protocol::SettingId::kTriggerMode, 2.0); // A frame per request.
    NSView* view = openView(unit);
    NSWindow* window = windowFor(view);
    WKWebView* webView = webViewOf(view);
    ASSERT_NE(webView, nil);
    ASSERT_TRUE(pumpUntil(
        [&] { return evaluate(webView, @"String(window.orizonDebug?.frameCount ?? 0)") != "0"; },
        kPageTimeout));

    const auto frameCount = [&] {
        return std::stoi("0" + evaluate(webView, @"String(window.orizonDebug?.frameCount ?? 0)"));
    };
    const int before = frameCount();

    AudioComponentInstanceDispose(unit);
    unit = nullptr;
    const auto engine = held.lock();
    ASSERT_NE(engine, nullptr) << "the view holds the engine";
    std::vector<float> tone(480, 0.5F);
    std::array<const float*, 2> channels{tone.data(), tone.data()};
    engine->captureRing().write(channels.data(), 2, static_cast<int>(tone.size()));
    evaluate(webView, @"window.orizonDebug.requestFrame(); ''");
    EXPECT_TRUE(pumpUntil([&] { return frameCount() > before; }, kPageTimeout))
        << "still drawing its engine";

    [view removeFromSuperview];
    EXPECT_FALSE(editorCoreOf(view)->workerRunning());
    [window close];
}

} // namespace
} // namespace orizon::au
