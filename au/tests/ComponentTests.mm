#include "ComponentDescription.h"
#include "MacPageTesting.h"

#import <AudioToolbox/AUCocoaUIView.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Cocoa/Cocoa.h>
#include <array>
#include <gtest/gtest.h>
#include <objc/runtime.h>
#include <string>

// The component as a host meets it: this test process is an app of its own that does not link the
// Audio Unit, and loads Sineless.component (ORIZON_TEST_COMPONENT) as hosts load installed ones.

#define EXPECT_NSEQUAL(actual, expected)                                                           \
    EXPECT_TRUE([(actual) isEqual:(expected)])                                                     \
        << [[NSString stringWithFormat:@"%@ is not %@", (actual), (expected)] UTF8String]

namespace orizon::au {
namespace {

using testing::kPageTimeout;
using testing::pumpUntil;

std::string standardPath(NSString* path)
{
    return path.stringByStandardizingPath.stringByResolvingSymlinksInPath.UTF8String;
}

// A four-character code as Info.plist writes it.
NSString* fourCharacters(OSType code)
{
    const std::array<char, 5> text{static_cast<char>(code >> 24U), static_cast<char>(code >> 16U),
                                   static_cast<char>(code >> 8U), static_cast<char>(code), '\0'};
    return @(text.data());
}

// The component's version as hosts compare it: 0xMMMMmmbb from "major.minor.patch".
unsigned long versionCode(NSString* version)
{
    NSArray<NSString*>* parts = [version componentsSeparatedByString:@"."];
    const auto part = [&](NSUInteger index) {
        return static_cast<unsigned long>(parts[index].integerValue);
    };
    return (part(0) << 16U) | (part(1) << 8U) | part(2);
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

// Hosts find the component through its Info.plist: one entry with the codes the Audio Unit
// registers with, a name in the "Maker: Product" form hosts group by, the factory it exports and
// its version (hosts rescan and Logic revalidates when that changes), as an effect. Its bundle
// identifier is its own, never the VST3 plugin's, so that looking one up never finds the other.
TEST(Component, NamesItselfToHosts)
{
    NSBundle* component = [NSBundle bundleWithPath:@ORIZON_TEST_COMPONENT];
    ASSERT_NE(component, nil);
    EXPECT_GT(component.bundleIdentifier.length, 0U);
    EXPECT_FALSE([component.bundleIdentifier isEqual:@ORIZON_TEST_VST3_IDENTIFIER]);
    EXPECT_NSEQUAL(component.infoDictionary[@"CFBundleShortVersionString"], @ORIZON_TEST_VERSION);
    NSArray<NSDictionary*>* entries = component.infoDictionary[@"AudioComponents"];
    ASSERT_EQ(entries.count, 1U);
    NSDictionary* entry = entries[0];
    EXPECT_NSEQUAL(entry[@"type"], fourCharacters(kComponentDescription.componentType));
    EXPECT_NSEQUAL(entry[@"subtype"], fourCharacters(kComponentDescription.componentSubType));
    EXPECT_NSEQUAL(entry[@"manufacturer"],
                   fourCharacters(kComponentDescription.componentManufacturer));
    NSArray<NSString*>* name = [entry[@"name"] componentsSeparatedByString:@": "];
    ASSERT_EQ(name.count, 2U) << [entry[@"name"] UTF8String];
    EXPECT_GT(name[0].length, 0U) << "the maker";
    EXPECT_GT(name[1].length, 0U) << "the product";
    EXPECT_NSEQUAL(entry[@"factoryFunction"], @(kFactoryFunctionName));
    EXPECT_EQ([entry[@"version"] unsignedLongValue], versionCode(@ORIZON_TEST_VERSION));
    EXPECT_TRUE([entry[@"tags"] containsObject:@"Effects"]);
}

// The licences travel with the component: Sineless's, the embedded fonts', and those of the
// third-party code compiled into it (nlohmann_json's, the Audio Unit SDK's), whose licences ask
// that their text go with the binary.
TEST(Component, CarriesItsLicences)
{
    NSBundle* component = [NSBundle bundleWithPath:@ORIZON_TEST_COMPONENT];
    ASSERT_NE(component, nil);
    for (NSString* name :
         @[ @"LICENSE", @"OFL-Roboto.txt", @"OFL-RobotoMono.txt", @"LICENSE-nlohmann-json.txt" ]) {
        EXPECT_NE([component pathForResource:name ofType:nil], nil) << name.UTF8String;
    }
    NSString* sdk = [component pathForResource:@"LICENSE-AudioUnitSDK.txt" ofType:nil];
    ASSERT_NE(sdk, nil);
    NSString* text = [NSString stringWithContentsOfFile:sdk
                                               encoding:NSUTF8StringEncoding
                                                  error:nil];
    EXPECT_TRUE([text containsString:@"Apache License"]);
}

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
