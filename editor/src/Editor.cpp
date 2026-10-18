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
#if defined(_WIN32)
    return type != nullptr && std::strcmp(type, kPlatformTypeHWND) == 0;
#elif defined(__APPLE__)
    return type != nullptr && std::strcmp(type, kPlatformTypeNSView) == 0;
#else
    (void)type;
    return false; // No editor on Linux (src/linux/PlatformLinux.cpp).
#endif
}

} // namespace

Editor::Editor(std::shared_ptr<engine::Engine> engine, MessageObserver observer)
    : core(std::move(engine), std::move(observer))
{
    core.setSizeRequest([this](int width, int height) {
        const ViewRect current = getRect();
        requestSize({current.left, current.top, current.left + width, current.top + height});
    });
    setRect(hostRect());
}

ViewRect Editor::hostRect() const
{
    const ViewRect current = getRect();
    return {current.left, current.top, current.left + core.width(), current.top + core.height()};
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
    const bool scaleChanged = core.takeScaleOf(parent);
    if (scaleChanged) {
        setRect(hostRect());
    }
    core.attach(parent);
    const tresult result = CPluginView::attached(parent, type);
    if (scaleChanged) {
        requestSize(getRect());
    }
    return result;
}

void Editor::requestSize(ViewRect wanted)
{
    if (plugFrame == nullptr) {
        return;
    }
    sizedByHost = false;
    if (plugFrame->resizeView(this, &wanted) == kResultTrue && !sizedByHost) {
        onSize(&wanted);
    }
}

tresult PLUGIN_API Editor::removed()
{
    core.detach();
    return CPluginView::removed();
}

tresult PLUGIN_API Editor::onSize(ViewRect* newSize)
{
    if (newSize == nullptr) {
        return kInvalidArgument;
    }
    sizedByHost = true;
    setRect(*newSize);
    core.setSize(newSize->getWidth(), newSize->getHeight());
    return kResultTrue;
}

tresult PLUGIN_API Editor::canResize()
{
    return kResultTrue;
}

tresult PLUGIN_API Editor::checkSizeConstraint(ViewRect* size)
{
    if (size == nullptr) {
        return kInvalidArgument;
    }
    size->right = size->left + std::max(size->getWidth(), core.toHostUnits(kMinimumWidth));
    size->bottom = size->top + std::max(size->getHeight(), core.toHostUnits(minimumHeight()));
    return kResultTrue;
}

} // namespace orizon::editor
