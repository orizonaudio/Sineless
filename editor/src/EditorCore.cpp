#include "editor/EditorCore.h"

#include "editor/DisplayScale.h"
#include "engine/ChannelViews.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace orizon::editor {

namespace {

// How often host state changes (setState) are reported to the page.
constexpr std::chrono::milliseconds kStateSyncInterval{100};

// The largest size the resize grip may ask for, in CSS pixels on either side: beyond any display,
// and small enough that the size in window units stays well within an int.
constexpr double kMaximumGripSize = 16384.0;

// A size the page gave, or none. JSON numbers are finite.
std::optional<double> dimension(const nlohmann::json& message, const char* name)
{
    if (!message.contains(name) || !message[name].is_number()) {
        return std::nullopt;
    }
    return message[name].get<double>();
}

} // namespace

EditorCore::EditorCore(std::shared_ptr<engine::Engine> engine, MessageObserver observer)
    : observer(std::move(observer)), scale(displayScale(nullptr)), engineState(std::move(engine))
{
    // The size the editor was left at, while the plugin stays loaded.
    if (const auto size = engineState ? engineState->editorSize() : std::nullopt) {
        cssWidth = std::max(size->width, static_cast<double>(kMinimumWidth));
        cssHeight = std::max(size->height, static_cast<double>(kMinimumHeight));
    }
    windowWidth = width();
    windowHeight = height();
}

EditorCore::~EditorCore()
{
    detach();
}

int EditorCore::toHostUnits(double cssPixels) const
{
    return static_cast<int>(std::ceil(cssPixels * scale - 1e-6));
}

bool EditorCore::takeScaleOf(void* window)
{
    const double windowScale = displayScale(window);
    if (windowScale == scale) {
        return false;
    }
    scale = windowScale;
    // Before attaching, the editor's own size is the window's (the format's view sets it).
    windowWidth = width();
    windowHeight = height();
    return true;
}

bool EditorCore::refreshScale()
{
    if (parent == nullptr) {
        return false;
    }
    const double windowScale = displayScale(parent);
    if (windowScale == scale) {
        return false;
    }
    scale = windowScale;
    if (view) {
        view->setScale(scale);
    }
    return true;
}

void EditorCore::attach(void* window)
{
    parent = window;
    // Each frame request from the page is its credit for one frame.
    view = WebView::create(
        parent, windowWidth, windowHeight,
        [this](WebView& /*view*/, std::string_view message) { receive(message); },
        [this](WebView& /*view*/) {
            if (frameFlow) {
                frameFlow->grantCredit();
            }
            if (worker) {
                worker->wake();
            }
        },
        cursors);
    view->setScale(scale);
    transport = std::make_unique<WebViewTransport>(*view);
    dispatcher = UiDispatcher::create();
    dispatcher->every(kStateSyncInterval, [this] {
        if (session) {
            session->sync();
            requestMinimumHeight();
            if (minimumCheck) {
                minimumCheck();
            }
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
}

void EditorCore::detach()
{
    worker.reset();     // Joins the thread before the state and view it uses go away.
    dispatcher.reset(); // Drops queued deliveries and the state-sync timer.
    session.reset();
    frameFlow.reset();
    transport.reset();
    analyzer.reset();
    view.reset();
    parent = nullptr;
}

void EditorCore::updateScale()
{
    if (view == nullptr || !refreshScale()) {
        return;
    }
    // The same CSS size in the new window units, if they differ from the window's.
    if ((width() != windowWidth || height() != windowHeight) && sizeRequest) {
        sizeRequest(width(), height());
    }
}

void EditorCore::receive(std::string_view message)
{
    if (observer) {
        observer(message);
    }
    const auto parsed = WebViewTransport::parse(message);
    if (!parsed) {
        return;
    }
    const auto& type = (*parsed)["type"].get_ref<const std::string&>();
    if (type == messageName(protocol::MessageType::kResize)) {
        resize(*parsed);
    } else if (session) {
        session->receive(*parsed);
    } else if (type == messageName(protocol::MessageType::kHello) && transport) {
        // No engine: tell the page, so it can explain instead of waiting forever.
        transport->sendMessage({{"type", messageName(protocol::MessageType::kError)},
                                {"code", "engineUnavailable"},
                                {"message", "The editor cannot reach the plugin's engine."}});
    }
}

void EditorCore::setSize(int newWidth, int newHeight)
{
    // A host that follows a display change resizes the window itself, possibly before the next
    // check of the scale: the size is in the new window units.
    refreshScale();
    windowWidth = newWidth;
    windowHeight = newHeight;
    cssWidth = newWidth / scale;
    cssHeight = newHeight / scale;
    if (engineState) {
        engineState->setEditorSize({.width = cssWidth, .height = cssHeight});
    }
    if (view) {
        view->setSize(newWidth, newHeight);
    }
}

void EditorCore::resize(const nlohmann::json& message)
{
    const auto askedWidth = dimension(message, "width");
    const auto askedHeight = dimension(message, "height");
    if (!askedWidth || !askedHeight || !sizeRequest) {
        return;
    }
    const double newWidth =
        std::clamp(*askedWidth, static_cast<double>(kMinimumWidth), kMaximumGripSize);
    const double newHeight =
        std::clamp(*askedHeight, static_cast<double>(minimumHeight()), kMaximumGripSize);
    sizeRequest(toHostUnits(newWidth), toHostUnits(newHeight));
}

int EditorCore::minimumHeight() const
{
    if (!analyzer || !engineState) {
        return kMinimumHeight;
    }
    return minimumHeightFor(engineState->settings().get(), engineState->inputChannels(),
                            analyzer->laneSet());
}

int EditorCore::minimumHeightFor(const engine::Settings& settings, int inputChannels,
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

void EditorCore::requestMinimumHeight()
{
    const int minimum = minimumHeight();
    if (minimum == lastMinimumHeight) {
        return;
    }
    lastMinimumHeight = minimum;
    if (!sizeRequest || windowHeight >= toHostUnits(minimum)) {
        return;
    }
    sizeRequest(windowWidth, toHostUnits(minimum));
}

} // namespace orizon::editor
