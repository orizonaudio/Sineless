#include "editor/Editor.h"

#include "pluginterfaces/gui/iplugview.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

namespace orizon::editor {

using namespace Steinberg;

namespace {

// How often host state changes (setState) are reported to the page.
constexpr std::chrono::milliseconds kStateSyncInterval{100};

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
    // Each frame request from the page is its credit for one frame.
    view = WebView::create(
        parent, getRect().getWidth(), getRect().getHeight(),
        [this](WebView& /*view*/, std::string_view message) { receive(message); },
        [this](WebView& /*view*/) {
            if (frameFlow) {
                frameFlow->grantCredit();
            }
            if (worker) {
                worker->wake();
            }
        });
    transport = std::make_unique<WebViewTransport>(*view);
    if (engineState) {
        analyzer = std::make_unique<engine::Analyzer>(*engineState);
        dispatcher = UiDispatcher::create();
        frameFlow = std::make_unique<FrameFlow>(
            *analyzer, *dispatcher,
            [this](std::span<const std::byte> frame) { transport->sendFrame(frame); });
        session = std::make_unique<Session>(*engineState, *analyzer, *transport);
        dispatcher->every(kStateSyncInterval, [this] { session->sync(); });
        worker = std::make_unique<engine::AnalysisWorker>([this] { frameFlow->step(); });
    }
    return CPluginView::attached(parent, type);
}

tresult PLUGIN_API Editor::removed()
{
    worker.reset();     // Joins the thread before the state and view it uses go away.
    dispatcher.reset(); // Drops queued deliveries and the state-sync timer.
    session.reset();
    frameFlow.reset();
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
    const auto parsed = WebViewTransport::parse(message);
    if (!parsed) {
        return;
    }
    if (session) {
        session->receive(*parsed);
    } else if ((*parsed)["type"] == "hello" && transport) {
        // No engine: tell the page, so it can explain instead of waiting forever.
        transport->sendMessage({{"type", "error"},
                                {"code", "engineUnavailable"},
                                {"message", "The editor cannot reach the plugin's engine."}});
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
