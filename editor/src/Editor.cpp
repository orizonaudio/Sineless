#include "editor/Editor.h"

#include "pluginterfaces/gui/iplugview.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace orizon::editor {

using namespace Steinberg;

namespace {

bool isSupportedPlatform(FIDString type)
{
    return type != nullptr && std::strcmp(type, kPlatformTypeNSView) == 0;
}

} // namespace

Editor::Editor(WebView::MessageHandler onMessage) : onMessage(std::move(onMessage))
{
    ViewRect initial(0, 0, kDefaultWidth, kDefaultHeight);
    setRect(initial);
}

tresult PLUGIN_API Editor::isPlatformTypeSupported(FIDString type)
{
    return isSupportedPlatform(type) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API Editor::attached(void* parent, FIDString type)
{
    if (parent == nullptr || !isSupportedPlatform(type)) {
        return kResultFalse;
    }
    view = WebView::create(parent, getRect().getWidth(), getRect().getHeight(), onMessage);
    return CPluginView::attached(parent, type);
}

tresult PLUGIN_API Editor::removed()
{
    view.reset();
    return CPluginView::removed();
}

tresult PLUGIN_API Editor::onSize(ViewRect* newSize)
{
    if (newSize == nullptr) {
        return kInvalidArgument;
    }
    setRect(*newSize);
    if (view) {
        view->setSize(newSize->getWidth(), newSize->getHeight());
    }
    return kResultTrue;
}

tresult PLUGIN_API Editor::canResize()
{
    return kResultTrue;
}

tresult PLUGIN_API Editor::checkSizeConstraint(ViewRect* rect)
{
    if (rect == nullptr) {
        return kInvalidArgument;
    }
    rect->right = rect->left + std::max(rect->getWidth(), kMinimumWidth);
    rect->bottom = rect->top + std::max(rect->getHeight(), kMinimumHeight);
    return kResultTrue;
}

} // namespace orizon::editor
