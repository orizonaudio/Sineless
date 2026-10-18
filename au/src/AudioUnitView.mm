#include "AudioUnitView.h"

#include "AudioUnit.h"
#include "AudioUnitViewTesting.h"
#include "editor/EditorCore.h"
#include "engine/EngineRegistry.h"

#import <AudioToolbox/AUCocoaUIView.h>
#import <Cocoa/Cocoa.h>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <objc/runtime.h>
#include <optional>
#include <utility>

// The classes' names come from ORIZON_AU_CLASS_PREFIX, which the build makes unique to it (see
// au/CMakeLists.txt).
#define ORIZON_JOIN_(a, b) a##b
#define ORIZON_JOIN(a, b) ORIZON_JOIN_(a, b)
#define ORIZON_STRING_(a) #a
#define ORIZON_STRING(a) ORIZON_STRING_(a)
#define ORIZON_AU_VIEW_FACTORY ORIZON_JOIN(ORIZON_AU_CLASS_PREFIX, _ViewFactory)
#define ORIZON_AU_VIEW ORIZON_JOIN(ORIZON_AU_CLASS_PREFIX, _View)

// The view a host shows in the plugin's window: the editor filling it. The editor opens when the
// view is shown in a window and closes when it leaves; hosts make a new view each time they open
// the window and release it when they close it, which closes the editor too.
@interface ORIZON_AU_VIEW : NSView
- (instancetype)initWithEngine:(std::shared_ptr<orizon::engine::Engine>)engine;
- (orizon::editor::EditorCore*)editorCore;
@end

@implementation ORIZON_AU_VIEW {
    std::unique_ptr<orizon::editor::EditorCore> core;
    bool attached;
    bool restorePending;
    // Between the live resize's start and end (the user dragging the window), as AppKit tells
    // every view in the window.
    bool liveResizing;
    // The last size below the minimum that the view returned from, or none yet. A host that sets
    // it again keeps it; a user's drag starts afresh, since a window stops at the same smallest
    // size every time.
    std::optional<NSSize> refusedSize;
}

- (instancetype)initWithEngine:(std::shared_ptr<orizon::engine::Engine>)engine
{
    auto editor = std::make_unique<orizon::editor::EditorCore>(std::move(engine));
    self = [super initWithFrame:NSMakeRect(0, 0, editor->width(), editor->height())];
    if (self != nil) {
        core = std::move(editor);
        // Resizable: hosts read the mask to decide whether the window may be resized.
        self.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        if (@available(macOS 14.0, *)) {
            self.clipsToBounds = YES; // No longer the default from macOS 14.
        }
        // The editor asks for room (stacked lanes): the view takes the size, and the host follows
        // the view's frame.
        __weak ORIZON_AU_VIEW* weakSelf = self;
        core->setSizeRequest([weakSelf](int width, int height) {
            [weakSelf setFrameSize:NSMakeSize(width, height)];
        });
    }
    return self;
}

- (void)dealloc
{
    if (attached) {
        core->detach();
    }
}

- (BOOL)isFlipped
{
    return YES;
}

// Every resize, the host's (by frame or autoresizing) and the editor's own, comes through here.
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    if (core) {
        core->setSize(static_cast<int>(size.width), static_cast<int>(size.height));
        [self keepMinimumSize];
    }
}

// The editor's minimum: its width, and its height with the stacked lanes on show.
- (NSSize)minimumSize
{
    return NSMakeSize(core->toHostUnits(orizon::editor::EditorCore::kMinimumWidth),
                      core->toHostUnits(core->minimumHeight()));
}

// AUv2 has no way to refuse a size. Below the editor's minimum the view takes the host's size for
// the moment (the page keeps its minimum layout, clipped) and returns to the minimum once the
// resize is over: when the user's drag ends, or on the next turn of the run loop for a size the
// host set itself. Never from inside the host's resize, which could loop with it.
- (void)keepMinimumSize
{
    const NSSize minimum = [self minimumSize];
    const NSSize size = self.frame.size;
    if (restorePending || (size.width >= minimum.width && size.height >= minimum.height)) {
        return;
    }
    restorePending = true;
    __weak ORIZON_AU_VIEW* weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
      [weakSelf restoreMinimumSize];
    });
}

- (void)restoreMinimumSize
{
    restorePending = false;
    const NSSize minimum = [self minimumSize];
    const NSSize size = self.frame.size;
    if (liveResizing || (size.width >= minimum.width && size.height >= minimum.height) ||
        (refusedSize && NSEqualSizes(size, *refusedSize))) {
        return; // Being dragged, resized again meanwhile, or the host insists on this size.
    }
    refusedSize = size;
    [self setFrameSize:NSMakeSize(std::max(size.width, minimum.width),
                                  std::max(size.height, minimum.height))];
}

- (void)viewWillStartLiveResize
{
    [super viewWillStartLiveResize];
    liveResizing = true;
    refusedSize.reset();
}

- (void)viewDidEndLiveResize
{
    [super viewDidEndLiveResize];
    liveResizing = false;
    [self keepMinimumSize];
}

- (void)viewDidMoveToWindow
{
    [super viewDidMoveToWindow];
    if (self.window != nil && !attached) {
        core->attach((__bridge void*)self);
        attached = true;
    } else if (self.window == nil && attached) {
        core->detach();
        attached = false;
    }
}

- (orizon::editor::EditorCore*)editorCore
{
    return core.get();
}
@end

// What hosts ask for a view, named in kAudioUnitProperty_CocoaUI.
@interface ORIZON_AU_VIEW_FACTORY : NSObject <AUCocoaUIBase>
@end

@implementation ORIZON_AU_VIEW_FACTORY

- (unsigned)interfaceVersion
{
    return 0;
}

- (NSString*)description
{
    return @"Sineless";
}

// The size a host asks for is a hint only (some send nonsense): the editor has its own. The view
// finds the instance's engine by the token the Audio Unit gives; with none (not Sineless, or the
// token gone), the editor shows that it has no data.
- (NSView*)uiViewForAudioUnit:(AudioUnit)unit withSize:(NSSize)size
{
    (void)size;
    std::int64_t token = 0;
    UInt32 tokenSize = sizeof token;
    std::shared_ptr<orizon::engine::Engine> engine;
    if (AudioUnitGetProperty(unit, orizon::au::kEngineTokenProperty, kAudioUnitScope_Global, 0,
                             &token, &tokenSize) == noErr &&
        tokenSize == sizeof token) {
        engine = orizon::engine::EngineRegistry::find(token);
    }
    return [[ORIZON_AU_VIEW alloc] initWithEngine:std::move(engine)];
}

@end

namespace orizon::au {

const char* viewFactoryClassName()
{
    return ORIZON_STRING(ORIZON_AU_VIEW_FACTORY);
}

std::optional<std::string> bundlePathFor(std::string_view imagePath)
{
    constexpr std::string_view kExecutableDirectory = "/Contents/MacOS/";
    const std::size_t at = imagePath.rfind(kExecutableDirectory);
    if (at == std::string_view::npos || at == 0) {
        return std::nullopt;
    }
    const std::string_view executable = imagePath.substr(at + kExecutableDirectory.size());
    if (executable.empty() || executable.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(imagePath.substr(0, at));
}

OSStatus copyCocoaViewInfo(AudioUnitCocoaViewInfo& info)
{
    // The bundle of the image that holds the factory: this copy's, even with another copy of
    // Sineless (same bundle id) loaded in the host.
    const char* image = class_getImageName([ORIZON_AU_VIEW_FACTORY class]);
    return copyCocoaViewInfo(image != nullptr ? std::string_view(image) : std::string_view(), info);
}

OSStatus copyCocoaViewInfo(std::string_view imagePath, AudioUnitCocoaViewInfo& info)
{
    const std::optional<std::string> bundle = bundlePathFor(imagePath);
    if (!bundle) {
        return kAudioUnitErr_InvalidProperty;
    }
    CFURLRef location = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(bundle->data()),
        static_cast<CFIndex>(bundle->size()), true);
    if (location == nullptr) {
        return kAudioUnitErr_InvalidProperty;
    }
    info.mCocoaAUViewBundleLocation = location;
    info.mCocoaAUViewClass[0] =
        CFStringCreateWithCString(nullptr, viewFactoryClassName(), kCFStringEncodingUTF8);
    return noErr;
}

editor::EditorCore* editorCoreOf(NSView* view)
{
    if (![view isKindOfClass:[ORIZON_AU_VIEW class]]) {
        return nullptr;
    }
    return [static_cast<ORIZON_AU_VIEW*>(view) editorCore];
}

} // namespace orizon::au
