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

Editor::Editor(std::shared_ptr<engine::Engine> engine, MessageObserver observer)
    : observer(std::move(observer)), engineState(std::move(engine))
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
    view =
        WebView::create(parent, getRect().getWidth(), getRect().getHeight(),
                        [this](WebView& /*view*/, std::string_view message) { receive(message); });
    if (engineState) {
        analyzer = std::make_unique<engine::Analyzer>(*engineState);
        transport = std::make_unique<WebViewTransport>(*view);
        session = std::make_unique<Session>(*engineState, *analyzer, *transport);
        // Frames reach the page once the credit-based flow is in place (T-023).
        worker = std::make_unique<engine::AnalysisWorker>([] {});
    }
    return CPluginView::attached(parent, type);
}

tresult PLUGIN_API Editor::removed()
{
    worker.reset(); // Joins the thread before the state and view it uses go away.
    session.reset();
    transport.reset();
    analyzer.reset();
    view.reset();
    return CPluginView::removed();
}

void Editor::receive(std::string_view message)
{
    if (observer) {
        observer(message);
    }
    if (session) {
        if (const auto parsed = WebViewTransport::parse(message)) {
            session->receive(*parsed);
        }
    }
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
