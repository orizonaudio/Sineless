// Linux has no editor: the plugin builds there for the checks alone, with stubs for the web view
// and the UI dispatcher (src/linux/PlatformLinux.cpp) that return none. That is safe only while
// the editor refuses every Linux window type, so that no host opens it and the stubs are never
// used.

#include "editor/Editor.h"
#include "engine/Engine.h"

#include <gtest/gtest.h>
#include <memory>

namespace orizon::editor {
namespace {

using namespace Steinberg;

TEST(EditorLinux, RefusesEveryWindowType)
{
    auto editor = owned(new Editor(std::make_shared<engine::Engine>()));
    int window = 0; // Stands in for an X11 window id the host would pass.
    for (const FIDString type :
         {kPlatformTypeX11EmbedWindowID, kPlatformTypeNSView, kPlatformTypeHWND}) {
        EXPECT_EQ(editor->isPlatformTypeSupported(type), kResultFalse) << type;
        EXPECT_EQ(editor->attached(&window, type), kResultFalse) << type;
    }
}

} // namespace
} // namespace orizon::editor
