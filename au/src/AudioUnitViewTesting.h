#pragma once

#import <Cocoa/Cocoa.h>

namespace orizon::editor {
class EditorCore;
}

namespace orizon::au {

// For tests: the editor inside a view the factory made, or null for any other view.
orizon::editor::EditorCore* editorCoreOf(NSView* view);

} // namespace orizon::au
