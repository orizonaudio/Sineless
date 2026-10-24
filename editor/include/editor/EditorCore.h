#pragma once

#include "editor/FrameFlow.h"
#include "editor/Session.h"
#include "editor/Transport.h"
#include "editor/UiDispatcher.h"
#include "editor/WebView.h"
#include "engine/AnalysisWorker.h"
#include "engine/Analyzer.h"
#include "engine/Engine.h"

#include <functional>
#include <memory>
#include <string_view>

namespace orizon::editor {

// The plugin window, whatever the plugin format: a WebView filling the host's window, the
// session with the page, the frame flow and the analysis worker, and the sizing rules. A format's
// view (the VST3 Editor) attaches it to the host's window and passes sizes both ways. Sizes are
// in CSS pixels where named so, otherwise in the host's window units, scaled by displayScale.
class EditorCore
{
public:
    // The narrowest window that fits the whole control strip and the top bar (modes, groups and
    // readouts) with their edge padding, in the embedded fonts; the editor tests check the page
    // against it. The page takes it as its minimum width too.
    static constexpr int kMinimumWidth = protocol::kMinimumEditorWidth;
    static constexpr int kMinimumHeight = 250;
    static constexpr int kDefaultWidth = kMinimumWidth;
    static constexpr int kDefaultHeight = 500;
    // Stacked lanes of the multi-track view need this much height each, for their label chip
    // and a clear trace, above the top bar and control strip (kChromeHeight, measured): the
    // minimum height grows with the lanes shown, and the editor asks the host for the room.
    static constexpr int kMinimumLaneHeight = protocol::kMinimumLaneHeight;
    static constexpr int kChromeHeight = protocol::kChromeHeight;

    // Sees every raw message from the page before the session handles it. For tests.
    using MessageObserver = std::function<void(std::string_view message)>;
    // Asks the host for a size in window units. The format's view applies it if the host agrees
    // (through setSize, as the host's own resizes are), and otherwise leaves it.
    using SizeRequest = std::function<void(int width, int height)>;
    // Called at each check of the minimum height (every state sync), whether or not more room
    // is asked for: for a format's view that gives the minimum to the host itself.
    using MinimumCheck = std::function<void()>;

    // `engine` is null when the format's glue could not reach the processor's engine.
    explicit EditorCore(std::shared_ptr<engine::Engine> engine, MessageObserver observer = {});
    ~EditorCore();
    EditorCore(const EditorCore&) = delete;
    EditorCore& operator=(const EditorCore&) = delete;

    void setSizeRequest(SizeRequest request) { sizeRequest = std::move(request); }
    void setMinimumCheck(MinimumCheck check) { minimumCheck = std::move(check); }
    // Who shows the page's cursors in the webview attach() opens (WebView::Cursors).
    void setCursors(WebView::Cursors shownBy) { cursors = shownBy; }

    // Takes the scale of `parent`, the host's window, before attaching to it: the host sized the
    // window before attaching, possibly at a guessed scale. True if the scale changed, so that
    // the size in window units changed too.
    bool takeScaleOf(void* parent);
    // Opens the editor in `parent` at the current size: the webview, the session and, with an
    // engine, the analysis worker.
    void attach(void* parent);
    // Closes it again; the worker is joined before anything it uses goes away.
    void detach();

    // The host gave the editor this size, in window units.
    void setSize(int width, int height);
    // The size in window units that the CSS size comes to at the current scale: what the editor
    // asks the host for.
    [[nodiscard]] int width() const { return toHostUnits(cssWidth); }
    [[nodiscard]] int height() const { return toHostUnits(cssHeight); }
    [[nodiscard]] int toHostUnits(double cssPixels) const;

    // The minimum height now, in CSS pixels: kMinimumHeight, or more with stacked lanes on show.
    [[nodiscard]] int minimumHeight() const;
    // The same for given settings, track channels and lanes: kMinimumLaneHeight for each stacked
    // trace drawn, above the chrome, and never less than kMinimumHeight.
    [[nodiscard]] static int minimumHeightFor(const engine::Settings& settings, int inputChannels,
                                              const engine::LaneSet& laneSet);

    // Host window units per CSS pixel.
    [[nodiscard]] double scaleFactor() const { return scale; }
    // False when the editor must show the "unavailable" state.
    [[nodiscard]] bool engineAvailable() const { return engineState != nullptr; }
    // True while attached with an engine: the analysis worker and its per-session state exist
    // only while the editor is open.
    [[nodiscard]] bool workerRunning() const { return worker != nullptr; }
    [[nodiscard]] bool analyzerAllocated() const { return analyzer != nullptr; }
    // The live webview while attached, otherwise null. For tests.
    [[nodiscard]] WebView* webView() const { return view.get(); }

private:
    void receive(std::string_view message);
    // The user dragged the resize grip: asks the host for that size, no smaller than the minimum.
    void resize(const nlohmann::json& message);
    // Asks the host for more height when the lanes on show no longer fit, once per change.
    void requestMinimumHeight();
    // Follows a change of display scaling (the window moved to another display): the same size
    // in CSS pixels, in the new window units.
    void updateScale();
    // Takes the window's current scale, if attached; true if it changed.
    bool refreshScale();

    MessageObserver observer;
    SizeRequest sizeRequest;
    MinimumCheck minimumCheck;
    WebView::Cursors cursors = WebView::Cursors::kShownByWebKit;
    void* parent = nullptr;
    double scale = 1.0;
    // The editor's size in CSS pixels, the truth that the size in window units follows: a scale
    // change applied to the size itself would apply twice if the host has resized it already.
    double cssWidth = kDefaultWidth;
    double cssHeight = kDefaultHeight;
    // The size the window has, in window units: the last the host gave, or the editor's own
    // before the host gives one. It differs from width() and height() while a host has not
    // followed a scale change, and decides whether more room must be asked for.
    int windowWidth = 0;
    int windowHeight = 0;
    int lastMinimumHeight = kMinimumHeight;
    std::shared_ptr<engine::Engine> engineState;
    std::unique_ptr<WebView> view;
    std::unique_ptr<WebViewTransport> transport;
    std::unique_ptr<UiDispatcher> dispatcher;
    std::unique_ptr<FrameFlow> frameFlow;
    std::unique_ptr<Session> session;
    std::unique_ptr<engine::Analyzer> analyzer;
    std::unique_ptr<engine::AnalysisWorker> worker; // Declared last: stopped before the rest.
};

} // namespace orizon::editor
