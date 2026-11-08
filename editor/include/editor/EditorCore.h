#pragma once

#include "editor/ChosenSize.h"
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
    // The narrowest window that fits the whole toolbar and status bar with their edge padding, in
    // the embedded fonts, the wordmark included; the editor tests check the page against it. The
    // page takes it as its minimum width too (docs/ui/README.md).
    static constexpr int kMinimumWidth = ChosenSize::kMinimumWidth;
    // The shortest window whose screen fits the tallest panel in its corner (limits.ts).
    static constexpr int kMinimumHeight = ChosenSize::kMinimumHeight;
    static constexpr int kDefaultWidth = ChosenSize::kDefaultWidth;
    static constexpr int kDefaultHeight = ChosenSize::kDefaultHeight;
    // Stacked lanes of the multi-track view need this much height each, for their label chip
    // and a clear trace, above the toolbar and status bar (kChromeHeight, measured): the
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
    // Opens a URL in the default browser: openInBrowser, unless a test gives its own.
    using LinkOpener = std::function<void(std::string_view url)>;

    // `engine` is null when the format's glue could not reach the processor's engine.
    explicit EditorCore(std::shared_ptr<engine::Engine> engine, MessageObserver observer = {});
    ~EditorCore();
    EditorCore(const EditorCore&) = delete;
    EditorCore& operator=(const EditorCore&) = delete;

    void setSizeRequest(SizeRequest request) { sizeRequest = std::move(request); }
    void setMinimumCheck(MinimumCheck check) { minimumCheck = std::move(check); }
    void setLinkOpener(LinkOpener opener) { linkOpener = std::move(opener); }
    // Who shows the page's cursors in the webview attach() opens (WebView::Cursors).
    void setCursors(WebView::Cursors shownBy) { cursors = shownBy; }
    // How the pointer hold in that webview hides the pointer (WebView::PointerHiding).
    void setPointerHiding(WebView::PointerHiding hiding) { pointerHiding = hiding; }
    // Whether editors opened from now on draw the traces natively where the platform can (macOS
    // 14+, AD-30); on by default. The showcase films the page alone, so it turns this off.
    static void setNativeTraces(bool enabled);
    // Whether this editor draws the traces natively. For tests.
    [[nodiscard]] bool drawsTracesNatively() const { return nativeTraces; }

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

    // True once the window has opened (the first state sync after attach): from then on, sizes
    // from the host may be the user's. For tests.
    [[nodiscard]] bool windowOpened() const { return chosen.opened(); }
    // Host window units per CSS pixel.
    [[nodiscard]] double scaleFactor() const { return scale; }
    // False when the editor must show the "unavailable" state.
    [[nodiscard]] bool engineAvailable() const { return engineState != nullptr; }
    // True while attached with an engine: the analysis worker and its per-session state exist
    // only while the editor is open.
    [[nodiscard]] bool workerRunning() const { return worker != nullptr; }
    [[nodiscard]] bool analyzerAllocated() const { return analyzer != nullptr; }
    // The display clock the analyzer is given while the traces are drawn natively, else null. For
    // tests.
    [[nodiscard]] const engine::FramePacing* displayClock() const { return pacing.get(); }
    // The live webview while attached, otherwise null. For tests.
    [[nodiscard]] WebView* webView() const { return view.get(); }

private:
    void receive(std::string_view message);
    // The user dragged the resize grip: asks the host for that size, no smaller than the minimum.
    void resize(const nlohmann::json& message);
    // The page asked for the vendor's contact page: opened after this message, if the build has
    // one.
    void openContactPage();
    // Asks the host for the size the window should have (ChosenSize::target) when the lanes'
    // minimum changes, growing or shrinking, and once as the window opens, at the first state sync
    // after attach, whatever the minimum: the host may have placed the window at a size of its own.
    void requestMinimumHeight();
    // Asks the host for `size`, in window units; from the user's drag of the grip or not.
    void request(WindowSize size, bool fromGrip);
    // Follows a change of display scaling (the window moved to another display): the same size
    // in CSS pixels, in the new window units.
    void updateScale();
    // Takes the window's current scale, if attached; true if it changed.
    bool refreshScale();
    // Draws the traces natively if the webview can (attach, before the worker starts).
    void startNativeTraces();
    [[nodiscard]] static bool frameBarcodeSwitch();

    MessageObserver observer;
    SizeRequest sizeRequest;
    MinimumCheck minimumCheck;
    LinkOpener linkOpener;
    WebView::Cursors cursors = WebView::Cursors::kShownByWebKit;
    WebView::PointerHiding pointerHiding = WebView::PointerHiding::kPublic;
    void* parent = nullptr;
    double scale = 1.0;
    // The size the user chose, which the window returns to; saved on the engine.
    ChosenSize chosen;
    // The window's size in CSS pixels, the truth that the size in window units follows: a scale
    // change applied to the size itself would apply twice if the host has resized it already.
    double cssWidth = kDefaultWidth;
    double cssHeight = kDefaultHeight;
    // The size the window has, in window units: the last the host gave, or the editor's own
    // before the host gives one. It differs from width() and height() while a host has not
    // followed a scale change, and decides whether more room must be asked for.
    int windowWidth = 0;
    int windowHeight = 0;
    int lastMinimumHeight = kMinimumHeight; // Read once the window has opened.
    std::shared_ptr<engine::Engine> engineState;
    std::unique_ptr<WebView> view;
    std::unique_ptr<WebViewTransport> transport;
    std::unique_ptr<UiDispatcher> dispatcher;
    std::unique_ptr<FrameFlow> frameFlow;
    std::unique_ptr<Session> session;
    std::unique_ptr<engine::Analyzer> analyzer;
    // The display clock the analyzer reads while the traces are drawn natively: before the worker,
    // so it outlives every read.
    std::unique_ptr<engine::FramePacing> pacing;
    bool nativeTraces = false;
    std::unique_ptr<engine::AnalysisWorker> worker; // Declared last: stopped before the rest.
};

} // namespace orizon::editor
