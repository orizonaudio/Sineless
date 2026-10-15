#include "editor/Editor.h"

#include "editor/DisplayScale.h"
#include "engine/ChannelViews.h"
#include "pluginterfaces/gui/iplugview.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace orizon::editor {

using namespace Steinberg;

namespace {

// How often host state changes (setState) are reported to the page.
constexpr std::chrono::milliseconds kStateSyncInterval{100};

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
    : observer(std::move(observer)), scale(displayScale(nullptr)), engineState(std::move(engine))
{
    setRect(hostRect());
}

int Editor::toHostUnits(double cssPixels) const
{
    return static_cast<int>(std::ceil(cssPixels * scale - 1e-6));
}

ViewRect Editor::hostRect() const
{
    const ViewRect current = getRect();
    return {current.left, current.top, current.left + toHostUnits(cssWidth),
            current.top + toHostUnits(cssHeight)};
}

bool Editor::refreshScale()
{
    if (systemWindow == nullptr) {
        return false;
    }
    const double windowScale = displayScale(systemWindow);
    if (windowScale == scale) {
        return false;
    }
    scale = windowScale;
    if (view) {
        view->setScale(scale);
    }
    return true;
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
    // The host sized the window before attaching, possibly at a guessed scale; the window's
    // own scale is the true one.
    const double windowScale = displayScale(parent);
    const bool scaleChanged = windowScale != scale;
    if (scaleChanged) {
        scale = windowScale;
        setRect(hostRect());
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
    view->setScale(scale);
    transport = std::make_unique<WebViewTransport>(*view);
    dispatcher = UiDispatcher::create();
    dispatcher->every(kStateSyncInterval, [this] {
        if (session) {
            session->sync();
            requestMinimumHeight();
        }
        updateScale();
    });
    if (engineState) {
        analyzer = std::make_unique<engine::Analyzer>(*engineState);
        frameFlow = std::make_unique<FrameFlow>(
            *analyzer, *dispatcher,
            [this](std::span<const std::byte> frame) { transport->sendFrame(frame); });
        session = std::make_unique<Session>(*engineState, *analyzer, *transport);
        worker = std::make_unique<engine::AnalysisWorker>([this] { frameFlow->step(); });
    }
    const tresult result = CPluginView::attached(parent, type);
    if (scaleChanged) {
        requestSize(getRect());
    }
    return result;
}

void Editor::updateScale()
{
    if (view == nullptr || !refreshScale()) {
        return;
    }
    const ViewRect wanted = hostRect();
    const ViewRect current = getRect();
    if (wanted.getWidth() != current.getWidth() || wanted.getHeight() != current.getHeight()) {
        requestSize(wanted);
    }
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
    // A host that follows a display change resizes the window itself, possibly before the next
    // check of the scale: the size is in the new window units.
    refreshScale();
    sizedByHost = true;
    setRect(*newSize);
    cssWidth = newSize->getWidth() / scale;
    cssHeight = newSize->getHeight() / scale;
    if (view) {
        view->setSize(newSize->getWidth(), newSize->getHeight());
    }
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
    size->right = size->left + std::max(size->getWidth(), toHostUnits(kMinimumWidth));
    size->bottom = size->top + std::max(size->getHeight(), toHostUnits(minimumHeight()));
    return kResultTrue;
}

int Editor::minimumHeight() const
{
    if (!analyzer || !engineState) {
        return kMinimumHeight;
    }
    return minimumHeightFor(engineState->settings().get(), engineState->inputChannels(),
                            analyzer->laneSet());
}

int Editor::minimumHeightFor(const engine::Settings& settings, int inputChannels,
                             const engine::LaneSet& laneSet)
{
    using protocol::SettingId;
    const auto mode = settings.enumValue<protocol::DisplayMode>(SettingId::kDisplayMode);
    const bool stacked = settings.enumValue<protocol::ViewLayout>(SettingId::kViewLayout) ==
                         protocol::ViewLayout::kStacked;
    if (!stacked) {
        return kMinimumHeight;
    }
    int lanes = 0;
    if (mode == protocol::DisplayMode::kSingle) {
        // The channel views on show, one lane each.
        lanes = std::popcount(engine::effectiveViews(
            static_cast<std::uint8_t>(settings.value(SettingId::kChannelViews)), inputChannels));
    } else {
        // The tracks on show, and in the Compare view the sum lane, which is drawn whether or
        // not another track is checked (alone, it is this track).
        lanes = static_cast<int>(laneSet.lanes.size()) +
                (mode == protocol::DisplayMode::kCompare ? 1 : 0);
    }
    return std::max(kMinimumHeight, kChromeHeight + lanes * kMinimumLaneHeight);
}

void Editor::requestMinimumHeight()
{
    const int minimum = minimumHeight();
    if (minimum == lastMinimumHeight) {
        return;
    }
    lastMinimumHeight = minimum;
    if (plugFrame == nullptr || getRect().getHeight() >= toHostUnits(minimum)) {
        return;
    }
    ViewRect wanted = getRect();
    wanted.bottom = wanted.top + toHostUnits(minimum);
    requestSize(wanted);
}

} // namespace orizon::editor
