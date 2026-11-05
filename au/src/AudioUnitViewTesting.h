#pragma once

#import <Cocoa/Cocoa.h>

namespace orizon::editor {
class EditorCore;
}

namespace orizon::au {

// For tests: the editor inside a view the factory made, or null for any other view.
orizon::editor::EditorCore* editorCoreOf(NSView* view);

// For tests: how long a size the view gives itself holds against an Auto Layout host, in seconds.
double ownSizeHoldSeconds();

// For tests: the bundle identifier views made from now on take their host to have (nil: the real
// one, the process's main bundle's).
void setHostBundleIdForTests(NSString* bundleId);

// For tests: whether the view hides the pointer with the cursor-in-background setting in a host
// with this bundle identifier.
bool hidesPointerWithBackgroundCursorForTests(NSString* hostBundleId);

} // namespace orizon::au
