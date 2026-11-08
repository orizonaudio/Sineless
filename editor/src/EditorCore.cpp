#include "editor/EditorCore.h"

#include "Branding.h"
#include "editor/DisplayScale.h"
#include "editor/OpenLink.h"
#include "engine/ChannelViews.h"
#include "engine/MonotonicClock.h"
#include "engine/Settings.h"
#include "engine/Version.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace orizon::editor {

namespace {

// How often host state changes (setState) are reported to the page.
constexpr std::chrono::milliseconds kStateSyncInterval{100};

// Until the display link reports its screen's (before its first callback), the refresh rate the
// display clock assumes.
constexpr double kAssumedRefreshHertz = 60.0;

std::atomic<bool>& nativeTracesEnabled()
{
    static std::atomic<bool> enabled{true};
    return enabled;
}

// A test's display scale, read on the UI thread only; empty: the window's own.
EditorCore::ScaleSource& scaleSourceForTests()
{
    static EditorCore::ScaleSource source;
    return source;
}

double scaleOf(void* window)
{
    const EditorCore::ScaleSource& source = scaleSourceForTests();
    return source ? source(window) : displayScale(window);
}

// A size the page gave, or none. JSON numbers are finite.
std::optional<double> dimension(const nlohmann::json& message, const char* name)
{
    if (!message.contains(name) || !message[name].is_number()) {
        return std::nullopt;
    }
    return message[name].get<double>();
}

} // namespace

void EditorCore::setNativeTraces(bool enabled)
{
    nativeTracesEnabled().store(enabled);
}

void EditorCore::setDisplayScaleForTests(ScaleSource source)
{
    scaleSourceForTests() = std::move(source);
}

EditorCore::EditorCore(std::shared_ptr<engine::Engine> engine, MessageObserver observer)
    : observer(std::move(observer)), linkOpener(&openInBrowser), scale(scaleOf(nullptr)),
      chosen(engine ? engine->editorSize() : std::nullopt), cssWidth(chosen.width()),
      cssHeight(chosen.height()), engineState(std::move(engine))
{
    windowWidth = width();
    windowHeight = height();
}

EditorCore::~EditorCore()
{
    detach();
}

int EditorCore::toHostUnits(double cssPixels) const
{
    return toWindowUnits(cssPixels, scale);
}

bool EditorCore::takeScaleOf(void* window)
{
    const double windowScale = scaleOf(window);
    if (windowScale == scale) {
        return false;
    }
    chosen.scaleChanged(scale);
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
    const double windowScale = scaleOf(parent);
    if (windowScale == scale) {
        return false;
    }
    chosen.scaleChanged(scale);
    scale = windowScale;
    if (view) {
        view->setScale(scale);
    }
    return true;
}

void EditorCore::attach(void* window)
{
    parent = window;
    chosen.close(); // Until the first state sync: the host may still be placing the window.
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
        cursors, pointerHiding);
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
        session->setFrameBarcode(frameBarcodeSwitch());
        if (nativeTracesEnabled().load()) {
            startNativeTraces();
        }
        worker = std::make_unique<engine::AnalysisWorker>([this] { frameFlow->step(); });
    }
}

// The diagnostic switch, read as the editor opens: SINELESS_FRAME_BARCODE=1 in the host's
// environment (launchctl setenv on macOS, a user variable on Windows, then the host started) makes
// every picture of the traces carry the frame barcode, read back from screen recordings by
// scripts/frame-barcode.
bool EditorCore::frameBarcodeSwitch()
{
#ifdef _MSC_VER
    // MSVC deprecates std::getenv (C4996, an error at /W4 /WX); _dupenv_s is its replacement.
    char* value = nullptr;
    std::size_t length = 0;
    const bool set = _dupenv_s(&value, &length, "SINELESS_FRAME_BARCODE") == 0 && value != nullptr;
    const bool on = set && std::string_view(value) == "1";
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s allocates with malloc.
    return on;
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read on the UI thread; nothing here sets it.
    const char* value = std::getenv("SINELESS_FRAME_BARCODE");
    return value != nullptr && std::string_view(value) == "1";
#endif
}

// The display link asks for frames and places them by its clock; the page only takes copies. Set
// up before the worker starts, so the analyzer's one clock is installed before its first read.
void EditorCore::startNativeTraces()
{
    pacing = std::make_unique<engine::FramePacing>(kAssumedRefreshHertz);
    nativeTraces = view->drawTracesNatively(
        {.pacing = pacing.get(),
         .requestFrame =
             [this] {
                 frameFlow->requestForDisplay();
                 worker->wake();
             },
         .takeFrame =
             [this](std::vector<std::byte>& into) { return frameFlow->takeDisplayFrame(into); },
         .lost =
             [this] {
                 // The page draws nothing either: its fetch makes the frames again, so readouts,
                 // hover and the status bar stay live under the message.
                 frameFlow->setNative(false);
                 session->nativeGraphicsLost();
             },
         .frameBarcode = frameBarcodeSwitch()});
    if (!nativeTraces) {
        pacing.reset();
        return;
    }
    engine::FramePacing* clock = pacing.get();
    analyzer->setClock([clock] { return clock->clock(engine::monotonicNanos()); });
    frameFlow->setNative(true);
    session->setNativeTraces(true);
}

void EditorCore::detach()
{
    if (view) {
        view->stopNativeTraces(); // Asks the worker and the frame flow nothing after this.
    }
    worker.reset();     // Joins the thread before the state and view it uses go away.
    dispatcher.reset(); // Drops queued deliveries and the state-sync timer.
    session.reset();
    frameFlow.reset();
    transport.reset();
    analyzer.reset();
    pacing.reset();
    nativeTraces = false;
    view.reset();
    parent = nullptr;
    chosen.close();
}

void EditorCore::updateScale()
{
    if (view == nullptr || !refreshScale()) {
        return;
    }
    // The same CSS size in the new window units, if they differ from the window's.
    if (width() != windowWidth || height() != windowHeight) {
        request({.width = width(), .height = height()}, false);
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
    if (type == messageName(protocol::MessageType::kHello) && transport) {
        // First, and with or without an engine: the version a user reports.
        transport->sendMessage({{"type", messageName(protocol::MessageType::kAbout)},
                                {"version", engine::kVersionString},
                                {"contact", !std::string_view(branding::kContactUrl).empty()}});
    }
    if (type == messageName(protocol::MessageType::kResize)) {
        resize(*parsed);
    } else if (type == messageName(protocol::MessageType::kContact)) {
        openContactPage();
    } else if (type == messageName(protocol::MessageType::kTraceView)) {
        // A malformed one is refused, and the last good one kept (QS-7).
        if (const auto traceView = TraceView::parse(*parsed); traceView && nativeTraces) {
            view->setTraceView(*traceView);
        }
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
    if (chosen.take({.width = newWidth, .height = newHeight}, scale, minimumHeight()) &&
        engineState) {
        engineState->setEditorSize(
            engine::EditorSize{.width = chosen.width(), .height = chosen.height()});
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
        std::clamp(*askedWidth, static_cast<double>(kMinimumWidth), ChosenSize::kLargestSize);
    const double newHeight =
        std::clamp(*askedHeight, static_cast<double>(minimumHeight()), ChosenSize::kLargestSize);
    request({.width = toHostUnits(newWidth), .height = toHostUnits(newHeight)}, true);
}

void EditorCore::request(WindowSize size, bool fromGrip)
{
    if (!sizeRequest) {
        return;
    }
    chosen.requested(size, fromGrip); // First: the host may reply within the request.
    sizeRequest(size.width, size.height);
}

// After this message is handled, not from within it: WebView2 does not support re-entering it from
// its own event handlers, and the system may wait for the browser to start or show a dialog. Work
// posted never runs once the editor has closed.
void EditorCore::openContactPage()
{
    const std::string_view url = branding::kContactUrl;
    if (url.empty()) {
        return;
    }
    dispatcher->post([this, url] { linkOpener(url); });
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
        // The tracks on show, and with Compare on the sum lane, which is drawn whether or not
        // another track is checked (alone, it is this track).
        lanes = static_cast<int>(laneSet.lanes.size()) + (engine::compareShown(settings) ? 1 : 0);
    }
    return std::max(kMinimumHeight, kChromeHeight + lanes * kMinimumLaneHeight);
}

void EditorCore::requestMinimumHeight()
{
    const int minimum = minimumHeight();
    if (chosen.opened() && minimum == lastMinimumHeight) {
        return;
    }
    lastMinimumHeight = minimum;
    chosen.open();
    const WindowSize target = chosen.target(scale, minimum);
    if (target != WindowSize{.width = windowWidth, .height = windowHeight}) {
        request(target, false);
    }
}

} // namespace orizon::editor
