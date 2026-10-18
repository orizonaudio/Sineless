#include "ComponentDescription.h"
#include "MacPageTesting.h"

#import <AudioToolbox/AUCocoaUIView.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Cocoa/Cocoa.h>
#include <gtest/gtest.h>
#include <objc/runtime.h>
#include <string>

// The component as a host meets it: this test process is an app of its own that does not link the
// Audio Unit, and loads Sineless.component (ORIZON_TEST_COMPONENT) as hosts load installed ones.

namespace orizon::au {
namespace {

using testing::kPageTimeout;
using testing::pumpUntil;

std::string standardPath(NSString* path)
{
    return path.stringByStandardizingPath.stringByResolvingSymlinksInPath.UTF8String;
}

class ComponentTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite() { [NSApplication sharedApplication]; }

    // Loads the component and registers it through its factory function, as a host does with an
    // installed one.
    void SetUp() override
    {
        component = [NSBundle bundleWithPath:@ORIZON_TEST_COMPONENT];
        ASSERT_NE(component, nil);
        ASSERT_TRUE([component load]);
        CFBundleRef loaded = CFBundleCreate(nullptr, (__bridge CFURLRef)component.bundleURL);
        ASSERT_NE(loaded, nullptr);
        auto* factory =
            reinterpret_cast<AudioComponentFactoryFunction>(CFBundleGetFunctionPointerForName(
                loaded, (__bridge CFStringRef) @(kFactoryFunctionName)));
        CFRelease(loaded);
        ASSERT_NE(factory, nullptr) << "the component exports its factory";
        static AudioComponent registered = AudioComponentRegister(
            &kComponentDescription, CFSTR("Orizon Audio: Sineless"), 1, factory);
        ASSERT_NE(registered, nullptr);
        ASSERT_EQ(AudioComponentInstanceNew(registered, &unit), noErr);
    }

    void TearDown() override
    {
        if (unit != nullptr) {
            AudioComponentInstanceDispose(unit);
        }
    }

    NSBundle* component = nil;
    AudioUnit unit = nullptr;
};

// The view a host loads is the component's own: the Audio Unit names the component's bundle (not
// the host app's), the class it names is found there, and it makes a view, from the component's
// code, that shows the editor once the host puts it in a window. The host releases what it gets.
TEST_F(ComponentTest, OffersTheViewFromItsOwnBundle)
{
    AudioUnitCocoaViewInfo info{};
    UInt32 size = sizeof info;
    ASSERT_EQ(AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
                                   &info, &size),
              noErr);
    auto* location = (__bridge NSURL*)info.mCocoaAUViewBundleLocation;
    EXPECT_EQ(standardPath(location.path), standardPath(component.bundlePath));
    EXPECT_NE(standardPath(location.path), standardPath(NSBundle.mainBundle.bundlePath))
        << "not the host's own bundle";

    NSBundle* bundle = [NSBundle bundleWithURL:location];
    Class factoryClass = [bundle classNamed:(__bridge NSString*)info.mCocoaAUViewClass[0]];
    ASSERT_NE(factoryClass, nil);
    ASSERT_TRUE([factoryClass conformsToProtocol:@protocol(AUCocoaUIBase)]);
    NSView* view = nil;
    @autoreleasepool {
        id<AUCocoaUIBase> factory = [[factoryClass alloc] init];
        view = [factory uiViewForAudioUnit:unit withSize:NSMakeSize(400, 300)];
    }
    ASSERT_NE(view, nil);
    EXPECT_EQ(standardPath(@(class_getImageName([view class]))),
              standardPath(component.executablePath))
        << "the view is the component's";

    NSWindow* window = [[NSWindow alloc] initWithContentRect:view.frame
                                                   styleMask:NSWindowStyleMaskTitled
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    [window.contentView addSubview:view];
    EXPECT_TRUE(pumpUntil([&] { return view.subviews.count > 0; }, kPageTimeout))
        << "the editor opens in the window";
    [view removeFromSuperview];
    [window close];

    CFRelease(info.mCocoaAUViewBundleLocation);
    CFRelease(info.mCocoaAUViewClass[0]);
}

} // namespace
} // namespace orizon::au
